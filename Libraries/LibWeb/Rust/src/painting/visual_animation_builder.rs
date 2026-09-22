/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Builds the compositor animations of a keyframe effect from its resolved keyframes: the lowering
//! of style values into the values the compositor interpolates, the per-effect cache of those
//! values, the animations an effect has built and published, and the document-wide list the visual
//! context tree receives.

use std::ffi::c_void;

use crate::css::absolutize::{COLOR_SYNTAX_LEGACY, number_from_value};
use crate::css::css_enums::keyword;
use crate::css::css_pixels::CssPixels;
use crate::css::easing::Easing;
use crate::css::property_metadata::property_id;
use crate::css::serialize::{TRANSFORM_FUNCTION_PARAMETER_TYPES, transform_function};
use crate::css::style_value::{
    FILTER_KIND_BLUR, FILTER_KIND_COLOR, FILTER_KIND_DROP_SHADOW, FILTER_KIND_HUE_ROTATE, RetainedStyleValueData,
    StyleValueData, is_none_keyword,
};
use crate::css::table_group_builder::{
    TRANSFORM_PARAMETER_ANGLE, TRANSFORM_PARAMETER_LENGTH, TRANSFORM_PARAMETER_LENGTH_NONE,
    TRANSFORM_PARAMETER_LENGTH_PERCENTAGE, TRANSFORM_PARAMETER_NUMBER, TRANSFORM_PARAMETER_NUMBER_PERCENTAGE,
    angle_degrees, angle_radians, length_px_unrounded,
};
use crate::painting::filter_bytes::{FfiFilterFunction, FfiFilterFunctionKind};
use crate::painting::host::{
    FfiCompositorAnimationBuildOutcome, FfiCompositorAnimationHost, FfiCompositorAnimationKeyframe,
    FfiCompositorAnimationRequest, FfiCompositorAnimationTiming, FfiCompositorKeyframeValueState,
    FfiVisualAnimationTargetKind, FfiVisualAnimationTransformOperationKind, TARGETED_TRANSFORM_PROPERTY_ROTATE,
    TARGETED_TRANSFORM_PROPERTY_SCALE, TARGETED_TRANSFORM_PROPERTY_TRANSFORM, TARGETED_TRANSFORM_PROPERTY_TRANSLATE,
};
use crate::painting::visual_animation::{
    VisualAnimation, VisualAnimationKeyframe, VisualAnimationTransformOperation, VisualAnimationValue,
};
use libcompositing_rust::ffi::ffi_slice;
use libgfx_rust::{Color, ColorFilterType};

const TARGET_KIND_COUNT: usize = 4;

fn target_kind_index(kind: FfiVisualAnimationTargetKind) -> usize {
    kind as usize
}

/// The properties a compositor animation can drive; the transform family lowers into one list.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum AnimatedProperty {
    Opacity,
    BackgroundColor,
    Filter,
    Translate,
    Rotate,
    Scale,
    Transform,
}

impl AnimatedProperty {
    const TRANSFORM_FAMILY: [Self; 4] = [Self::Translate, Self::Rotate, Self::Scale, Self::Transform];

    fn property_id(self) -> u16 {
        match self {
            Self::Opacity => property_id::OPACITY,
            Self::BackgroundColor => property_id::BACKGROUND_COLOR,
            Self::Filter => property_id::FILTER,
            Self::Translate => property_id::TRANSLATE,
            Self::Rotate => property_id::ROTATE,
            Self::Scale => property_id::SCALE,
            Self::Transform => property_id::TRANSFORM,
        }
    }

    fn state_in(self, keyframe: &FfiCompositorAnimationKeyframe) -> FfiCompositorKeyframeValueState {
        match self {
            Self::Opacity => keyframe.opacity,
            Self::BackgroundColor => keyframe.background_color,
            Self::Filter => keyframe.filter,
            Self::Translate => keyframe.translate,
            Self::Rotate => keyframe.rotate,
            Self::Scale => keyframe.scale,
            Self::Transform => keyframe.transform,
        }
    }

    fn targeted_transform_flag(self) -> u8 {
        match self {
            Self::Translate => TARGETED_TRANSFORM_PROPERTY_TRANSLATE,
            Self::Rotate => TARGETED_TRANSFORM_PROPERTY_ROTATE,
            Self::Scale => TARGETED_TRANSFORM_PROPERTY_SCALE,
            Self::Transform => TARGETED_TRANSFORM_PROPERTY_TRANSFORM,
            _ => 0,
        }
    }
}

/// The main thread's answers to what the builder asks while lowering values.
pub struct Host<'a>(&'a FfiCompositorAnimationHost);

impl<'a> Host<'a> {
    pub fn new(host: &'a FfiCompositorAnimationHost) -> Self {
        Self(host)
    }

    fn resolved_keyframe_value(
        &self,
        keyframe_index: usize,
        property: AnimatedProperty,
        uses_underlying_style: bool,
    ) -> Option<RetainedStyleValueData> {
        // SAFETY: The host returns a retained style value or null, synchronously.
        let value = unsafe {
            (self.0.resolved_keyframe_value)(
                self.0.context,
                keyframe_index,
                property.property_id(),
                uses_underlying_style,
            )
        };
        if value.is_null() {
            return None;
        }
        // SAFETY: The host hands over one strong reference, which the wrapper releases.
        Some(unsafe { RetainedStyleValueData::from_retained_pointer(value.cast()) })
    }

    fn resolve_color(&self, value: &StyleValueData) -> Option<Color> {
        let mut color = Color::TRANSPARENT;
        // SAFETY: The value is live for the call and the host writes the color synchronously.
        let has_color =
            unsafe { (self.0.resolve_color)(self.0.context, std::ptr::from_ref(value).cast(), &raw mut color) };
        has_color.then_some(color)
    }
}

/// A build request with its keyframes in reach.
pub struct Request<'a> {
    ffi: &'a FfiCompositorAnimationRequest,
    keyframes: &'a [FfiCompositorAnimationKeyframe],
}

impl<'a> Request<'a> {
    /// # Safety
    ///
    /// The request's keyframe range must be live.
    pub unsafe fn new(ffi: &'a FfiCompositorAnimationRequest) -> Self {
        Self {
            ffi,
            // SAFETY: The caller guarantees the keyframe range is live.
            keyframes: unsafe { ffi_slice(ffi.keyframes, ffi.keyframe_count) },
        }
    }

    pub fn target_kind(&self) -> FfiVisualAnimationTargetKind {
        self.ffi.target_kind
    }

    pub fn layout_node(&self) -> crate::layout::node_data::NodeSlotId {
        self.ffi.layout_node
    }

    fn targets(&self, property: AnimatedProperty) -> bool {
        self.ffi.targeted_transform_properties & property.targeted_transform_flag() != 0
    }

    fn targeted_transform_property_count(&self) -> usize {
        self.ffi.targeted_transform_properties.count_ones() as usize
    }

    fn targets_only_transform(&self) -> bool {
        self.ffi.targets_only_transform
    }

    fn timing(&self) -> &FfiCompositorAnimationTiming {
        &self.ffi.timing
    }

    fn reference_box(&self) -> (f32, f32) {
        (self.ffi.reference_box_width, self.ffi.reference_box_height)
    }

    fn device_pixels_per_css_pixel(&self) -> f32 {
        self.ffi.device_pixels_per_css_pixel
    }

    fn cache_key(&self) -> KeyframeValueCacheKey {
        KeyframeValueCacheKey {
            key_frame_set_identity: self.ffi.key_frame_set_identity,
            target_style_generation: self.ffi.target_style_generation,
            style_environment_version: self.ffi.style_environment_version,
            reference_box_width: self.ffi.reference_box_width,
            reference_box_height: self.ffi.reference_box_height,
            device_pixels_per_css_pixel: self.ffi.device_pixels_per_css_pixel,
        }
    }
}

/// CSS pixels quantized the way a Length converts to them, scaled to device pixels.
fn device_pixels(css_pixels_unrounded: f64, device_pixels_per_css_pixel: f32) -> f32 {
    CssPixels::nearest_value_for(css_pixels_unrounded).to_float() * device_pixels_per_css_pixel
}

fn opacity_from_style_value(value: &StyleValueData) -> Option<f32> {
    let StyleValueData::OpacityValue { value } = value else {
        return None;
    };
    let StyleValueData::Number { value } = value.data() else {
        return None;
    };
    value.is_finite().then_some(*value as f32)
}

fn is_legacy_color_function(value: &StyleValueData) -> bool {
    matches!(value, StyleValueData::ColorFunction { color_base, .. } if color_base.color_syntax == COLOR_SYNTAX_LEGACY)
}

/// Legacy sRGB colors interpolate in gamma-encoded sRGB, which the compositor sampler matches. Modern
/// color syntaxes stay on the main thread until compositor values can retain their interpolation
/// color space, and currentcolor with them.
fn background_color_from_style_value(value: &StyleValueData, host: &Host) -> Option<Color> {
    if matches!(value, StyleValueData::Keyword { keyword: code } if *code == keyword::CURRENTCOLOR) {
        return None;
    }
    if matches!(value, StyleValueData::ColorFunction { .. }) && !is_legacy_color_function(value) {
        return None;
    }
    host.resolve_color(value)
}

fn filter_functions_from_style_value(
    value: &StyleValueData,
    device_pixels_per_css_pixel: f32,
    host: &Host,
) -> Option<Vec<FfiFilterFunction>> {
    if is_none_keyword(value) {
        return Some(Vec::new());
    }
    let StyleValueData::ValueList { values, .. } = value else {
        return None;
    };
    let plain = |kind, amount| FfiFilterFunction {
        kind,
        amount,
        offset_x: 0.0,
        offset_y: 0.0,
        color: Color::TRANSPARENT,
        color_operation: ColorFilterType::Brightness,
    };
    let mut functions = Vec::with_capacity(values.as_slice().len());
    for item in values.as_slice() {
        let StyleValueData::Filter {
            kind,
            color_operation,
            value,
        } = item.data()
        else {
            return None;
        };
        let function = match *kind {
            FILTER_KIND_BLUR => {
                // The radius quantizes from the single precision the C++ accessor returned it in.
                let radius = length_px_unrounded(value.data(), None)? as f32;
                plain(
                    FfiFilterFunctionKind::Blur,
                    CssPixels::nearest_value_for_f32(radius).to_float() * device_pixels_per_css_pixel,
                )
            }
            FILTER_KIND_DROP_SHADOW => {
                let StyleValueData::Shadow {
                    color,
                    offset_x,
                    offset_y,
                    blur_radius,
                    ..
                } = value.data()
                else {
                    return None;
                };
                // Gfx filters hold 8-bit sRGB colors. Modern color syntaxes and currentcolor stay on the
                // main thread until compositor filter values can retain their color space and syntax.
                let color = color.optional_data().filter(|color| is_legacy_color_function(color))?;
                let color = host.resolve_color(color)?;
                let resolve_length = |length: &StyleValueData| {
                    length_px_unrounded(length, None).map(|px| device_pixels(px, device_pixels_per_css_pixel))
                };
                FfiFilterFunction {
                    kind: FfiFilterFunctionKind::DropShadow,
                    amount: match blur_radius.optional_data() {
                        Some(radius) => resolve_length(radius)?,
                        None => 0.0,
                    },
                    offset_x: resolve_length(offset_x.data())?,
                    offset_y: resolve_length(offset_y.data())?,
                    color,
                    color_operation: ColorFilterType::Brightness,
                }
            }
            FILTER_KIND_HUE_ROTATE => plain(FfiFilterFunctionKind::HueRotate, angle_degrees(value.data())? as f32),
            FILTER_KIND_COLOR => FfiFilterFunction {
                color_operation: ColorFilterType::from_i32(i32::from(*color_operation))?,
                ..plain(
                    FfiFilterFunctionKind::Color,
                    number_from_value(value.data(), 1.0)? as f32,
                )
            },
            _ => return None,
        };
        functions.push(function);
    }
    Some(functions)
}

fn is_scale_operation(kind: FfiVisualAnimationTransformOperationKind) -> bool {
    use FfiVisualAnimationTransformOperationKind as Kind;
    matches!(
        kind,
        Kind::Scale | Kind::Scale3d | Kind::ScaleX | Kind::ScaleY | Kind::ScaleZ
    )
}

fn is_translate_or_scale_operation(kind: FfiVisualAnimationTransformOperationKind) -> bool {
    use FfiVisualAnimationTransformOperationKind as Kind;
    is_scale_operation(kind)
        || matches!(
            kind,
            Kind::Translate | Kind::Translate3d | Kind::TranslateX | Kind::TranslateY | Kind::TranslateZ
        )
}

/// The reference box length a translation argument's percentage resolves against.
fn percentage_basis(function: u8, argument_index: usize, reference_box: (f32, f32)) -> Option<f64> {
    let (width, height) = reference_box;
    match function {
        transform_function::TRANSLATE | transform_function::TRANSLATE3D => match argument_index {
            0 => Some(f64::from(width)),
            1 => Some(f64::from(height)),
            _ => None,
        },
        transform_function::TRANSLATE_X => Some(f64::from(width)),
        transform_function::TRANSLATE_Y => Some(f64::from(height)),
        _ => None,
    }
}

/// One transform function lowered to the operation the compositor interpolates, with lengths in
/// device pixels and angles in radians; none for a function the compositor cannot interpolate.
fn transform_operation(
    function: u8,
    values: &[RetainedStyleValueData],
    reference_box: (f32, f32),
    device_pixels_per_css_pixel: f32,
) -> Option<VisualAnimationTransformOperation> {
    use FfiVisualAnimationTransformOperationKind as Kind;
    let mut kind = match function {
        transform_function::TRANSLATE => Kind::Translate,
        transform_function::TRANSLATE3D => Kind::Translate3d,
        transform_function::TRANSLATE_X => Kind::TranslateX,
        transform_function::TRANSLATE_Y => Kind::TranslateY,
        transform_function::TRANSLATE_Z => Kind::TranslateZ,
        transform_function::SCALE => Kind::Scale,
        transform_function::SCALE3D => Kind::Scale3d,
        transform_function::SCALE_X => Kind::ScaleX,
        transform_function::SCALE_Y => Kind::ScaleY,
        transform_function::SCALE_Z => Kind::ScaleZ,
        transform_function::ROTATE | transform_function::ROTATE3D => Kind::Rotate,
        transform_function::ROTATE_X => Kind::RotateX,
        transform_function::ROTATE_Y => Kind::RotateY,
        transform_function::ROTATE_Z => Kind::RotateZ,
        transform_function::SKEW => Kind::Skew,
        transform_function::SKEW_X => Kind::SkewX,
        transform_function::SKEW_Y => Kind::SkewY,
        _ => return None,
    };
    let parameters = TRANSFORM_FUNCTION_PARAMETER_TYPES.get(function as usize)?;
    let mut lowered = Vec::with_capacity(values.len());
    for (index, value) in values.iter().enumerate() {
        let data = value.data();
        let lowered_value = match *parameters.get(index)? {
            TRANSFORM_PARAMETER_ANGLE => angle_radians(data)? as f32,
            TRANSFORM_PARAMETER_LENGTH | TRANSFORM_PARAMETER_LENGTH_NONE | TRANSFORM_PARAMETER_LENGTH_PERCENTAGE => {
                device_pixels(
                    length_px_unrounded(data, percentage_basis(function, index, reference_box))?,
                    device_pixels_per_css_pixel,
                )
            }
            TRANSFORM_PARAMETER_NUMBER | TRANSFORM_PARAMETER_NUMBER_PERCENTAGE => number_from_value(data, 1.0)? as f32,
            _ => return None,
        };
        if !lowered_value.is_finite() {
            return None;
        }
        lowered.push(lowered_value);
    }
    if function == transform_function::ROTATE3D {
        // Only a rotation about one axis interpolates as an angle.
        if lowered.len() != 4 {
            return None;
        }
        let axis = if lowered[0] != 0.0 && lowered[1] == 0.0 && lowered[2] == 0.0 {
            kind = Kind::RotateX;
            lowered[0]
        } else if lowered[0] == 0.0 && lowered[1] != 0.0 && lowered[2] == 0.0 {
            kind = Kind::RotateY;
            lowered[1]
        } else if lowered[0] == 0.0 && lowered[1] == 0.0 && lowered[2] != 0.0 {
            kind = Kind::RotateZ;
            lowered[2]
        } else {
            return None;
        };
        let angle = if axis < 0.0 { -lowered[3] } else { lowered[3] };
        lowered = vec![angle];
    }
    if kind == Kind::Translate && lowered.len() == 1 {
        kind = Kind::TranslateX;
    }
    let operation = VisualAnimationTransformOperation::new(kind, &lowered)?;
    operation.is_valid().then_some(operation)
}

/// The operations a transform-family property's value lowers to. A `none` value takes the shape of
/// the identity given, so that it interpolates with the other keyframes' operations, or the
/// property's own identity when the effect has no other shape to offer.
fn transform_operations_from_style_value(
    property: AnimatedProperty,
    value: &StyleValueData,
    reference_box: (f32, f32),
    device_pixels_per_css_pixel: f32,
    identity_shape: Option<&[VisualAnimationTransformOperation]>,
) -> Option<Vec<VisualAnimationTransformOperation>> {
    use FfiVisualAnimationTransformOperationKind as Kind;
    if is_none_keyword(value) {
        if let Some(shape) = identity_shape {
            return shape
                .iter()
                .map(|operation| {
                    let identity_value = if is_scale_operation(operation.kind) { 1.0 } else { 0.0 };
                    let values = vec![identity_value; operation.values().len()];
                    VisualAnimationTransformOperation::new(operation.kind, &values)
                })
                .collect();
        }
        return match property {
            AnimatedProperty::Translate => Some(vec![VisualAnimationTransformOperation::new(
                Kind::Translate,
                &[0.0, 0.0],
            )?]),
            AnimatedProperty::Rotate => Some(vec![VisualAnimationTransformOperation::new(Kind::Rotate, &[0.0])?]),
            AnimatedProperty::Scale => Some(vec![VisualAnimationTransformOperation::new(Kind::Scale, &[1.0, 1.0])?]),
            _ => None,
        };
    }

    let lower = |transformation: &StyleValueData| {
        let StyleValueData::Transformation {
            transform_function,
            values,
            ..
        } = transformation
        else {
            return None;
        };
        transform_operation(
            *transform_function,
            values.as_slice(),
            reference_box,
            device_pixels_per_css_pixel,
        )
    };
    if property != AnimatedProperty::Transform {
        return Some(vec![lower(value)?]);
    }
    let StyleValueData::ValueList { values, .. } = value else {
        return None;
    };
    if values.as_slice().is_empty() {
        return None;
    }
    values.as_slice().iter().map(|item| lower(item.data())).collect()
}

/// Whether the transform lists of at least two keyframes are the same shape of translations that
/// only ever move horizontally; such an animation cannot change what an observer sees vertically.
fn transform_lists_only_translate_horizontally<'a>(
    mut lists: impl Iterator<Item = &'a [VisualAnimationTransformOperation]>,
) -> bool {
    use FfiVisualAnimationTransformOperationKind as Kind;
    let Some(first) = lists.next() else {
        return false;
    };
    if !first
        .iter()
        .all(|operation| matches!(operation.kind, Kind::Translate | Kind::TranslateX))
    {
        return false;
    }
    let translate_y = |operation: &VisualAnimationTransformOperation| match operation.kind {
        Kind::Translate => operation.values().get(1).copied().unwrap_or(0.0),
        _ => 0.0,
    };
    let mut list_count = 1;
    for list in lists {
        list_count += 1;
        let same_shape = list.len() == first.len()
            && list
                .iter()
                .zip(first)
                .all(|(operation, shape)| operation.kind == shape.kind && translate_y(operation) == translate_y(shape));
        if !same_shape {
            return false;
        }
    }
    list_count >= 2
}

/// The transform lists of the keyframes that give the transform property a value of their own,
/// or none when one of them cannot be lowered or takes the underlying style.
fn own_transform_keyframe_operations(
    request: &Request,
    host: &Host,
) -> Option<Vec<Vec<VisualAnimationTransformOperation>>> {
    let mut lists = Vec::new();
    for (index, keyframe) in request.keyframes.iter().enumerate() {
        match AnimatedProperty::Transform.state_in(keyframe) {
            FfiCompositorKeyframeValueState::Absent => continue,
            FfiCompositorKeyframeValueState::UsesUnderlyingStyle => return None,
            FfiCompositorKeyframeValueState::Present => {}
        }
        let value = host.resolved_keyframe_value(index, AnimatedProperty::Transform, false)?;
        lists.push(transform_operations_from_style_value(
            AnimatedProperty::Transform,
            value.data(),
            request.reference_box(),
            request.device_pixels_per_css_pixel(),
            None,
        )?);
    }
    Some(lists)
}

/// Whether an effect that targets only the transform property only ever translates horizontally.
pub fn effect_only_translates_horizontally(request: &Request, host: &Host) -> bool {
    if !request.targets_only_transform() {
        return false;
    }
    let Some(lists) = own_transform_keyframe_operations(request, host) else {
        return false;
    };
    transform_lists_only_translate_horizontally(lists.iter().map(Vec::as_slice))
}

/// Whether the transforms an effect animates keep the axes in place: it does not rotate, and its
/// transform keyframes only translate and scale.
pub fn effect_transform_preserves_axes(request: &Request, host: &Host) -> bool {
    if request.targets(AnimatedProperty::Rotate) {
        return false;
    }
    if !request.targets(AnimatedProperty::Transform) {
        return true;
    }
    let Some(lists) = own_transform_keyframe_operations(request, host) else {
        return false;
    };
    lists.iter().all(|operations| {
        operations
            .iter()
            .all(|operation| is_translate_or_scale_operation(operation.kind))
    })
}

#[derive(Clone, Copy, PartialEq)]
struct KeyframeValueCacheKey {
    key_frame_set_identity: u64,
    target_style_generation: u64,
    style_environment_version: u64,
    reference_box_width: f32,
    reference_box_height: f32,
    device_pixels_per_css_pixel: f32,
}

/// The values of one target kind lowered from an effect's keyframes, kept while the keyframes and
/// the style they resolve against stay the same. A keyframe without a value of the kind holds none;
/// keyframes that cannot be lowered leave no values at all.
struct KeyframeValueCache {
    key: KeyframeValueCacheKey,
    values: Option<Vec<Option<VisualAnimationValue>>>,
}

/// The transform list a `none` value of each transform-family property takes, resolved once per
/// cache build when a keyframe needs it. The outer option is the memo, the inner the shape found.
type TransformIdentityShapes = [Option<Option<Vec<VisualAnimationTransformOperation>>>; 4];

fn build_keyframe_value_cache(request: &Request, host: &Host, key: KeyframeValueCacheKey) -> KeyframeValueCache {
    let mut identity_shapes: TransformIdentityShapes = Default::default();
    let values = request
        .keyframes
        .iter()
        .enumerate()
        .map(|(index, keyframe)| lower_keyframe(request, host, index, keyframe, &mut identity_shapes))
        .collect();
    KeyframeValueCache { key, values }
}

/// The value of the request's target kind one keyframe gives: none when the keyframe leaves the
/// kind to the effect to synthesize, and no outcome when the keyframe cannot be lowered.
fn lower_keyframe(
    request: &Request,
    host: &Host,
    index: usize,
    keyframe: &FfiCompositorAnimationKeyframe,
    identity_shapes: &mut TransformIdentityShapes,
) -> Option<Option<VisualAnimationValue>> {
    let kind = request.target_kind();
    let property = match kind {
        FfiVisualAnimationTargetKind::Opacity => AnimatedProperty::Opacity,
        FfiVisualAnimationTargetKind::BackgroundColor => AnimatedProperty::BackgroundColor,
        FfiVisualAnimationTargetKind::Filter => AnimatedProperty::Filter,
        FfiVisualAnimationTargetKind::Transform => {
            return lower_transform_keyframe(request, host, index, keyframe, identity_shapes);
        }
    };
    if property.state_in(keyframe) != FfiCompositorKeyframeValueState::Present {
        return Some(None);
    }
    let resolved = host.resolved_keyframe_value(index, property, false)?;
    let value = match kind {
        FfiVisualAnimationTargetKind::Opacity => {
            VisualAnimationValue::Opacity(opacity_from_style_value(resolved.data())?)
        }
        FfiVisualAnimationTargetKind::BackgroundColor => {
            VisualAnimationValue::BackgroundColor(background_color_from_style_value(resolved.data(), host)?)
        }
        _ => VisualAnimationValue::Filter(filter_functions_from_style_value(
            resolved.data(),
            request.device_pixels_per_css_pixel(),
            host,
        )?),
    };
    Some(Some(value))
}

fn lower_transform_keyframe(
    request: &Request,
    host: &Host,
    index: usize,
    keyframe: &FfiCompositorAnimationKeyframe,
    identity_shapes: &mut TransformIdentityShapes,
) -> Option<Option<VisualAnimationValue>> {
    let mut operations = Vec::new();
    for (property_index, property) in AnimatedProperty::TRANSFORM_FAMILY.iter().enumerate() {
        if !request.targets(*property) {
            continue;
        }
        let state = property.state_in(keyframe);
        if state == FfiCompositorKeyframeValueState::Absent {
            // A keyframe that leaves out the only transform property is synthesized by the effect;
            // one that leaves out some of several cannot be lowered.
            return (request.targeted_transform_property_count() == 1).then_some(None);
        }
        let uses_underlying_style = state == FfiCompositorKeyframeValueState::UsesUnderlyingStyle;
        let resolved = host.resolved_keyframe_value(index, *property, uses_underlying_style)?;
        let identity_shape = if is_none_keyword(resolved.data()) {
            identity_shapes[property_index]
                .get_or_insert_with(|| resolve_transform_identity_shape(request, host, *property))
                .as_deref()
        } else {
            None
        };
        operations.extend(transform_operations_from_style_value(
            *property,
            resolved.data(),
            request.reference_box(),
            request.device_pixels_per_css_pixel(),
            identity_shape,
        )?);
    }
    Some(Some(VisualAnimationValue::Transform(operations)))
}

/// The shape a `none` value of a transform-family property takes: that of the first keyframe
/// giving the property a value of its own that is not `none`.
fn resolve_transform_identity_shape(
    request: &Request,
    host: &Host,
    property: AnimatedProperty,
) -> Option<Vec<VisualAnimationTransformOperation>> {
    for (index, keyframe) in request.keyframes.iter().enumerate() {
        if property.state_in(keyframe) != FfiCompositorKeyframeValueState::Present {
            continue;
        }
        let Some(candidate) = host.resolved_keyframe_value(index, property, false) else {
            continue;
        };
        if is_none_keyword(candidate.data()) {
            continue;
        }
        return transform_operations_from_style_value(
            property,
            candidate.data(),
            request.reference_box(),
            request.device_pixels_per_css_pixel(),
            None,
        );
    }
    None
}

/// What one keyframe effect has built for the compositor: the values it lowered from its
/// keyframes, the animations built in the current update pass, and the ones it published last.
#[derive(Default)]
pub struct CompositorAnimationEffectState {
    keyframe_value_caches: [Option<KeyframeValueCache>; TARGET_KIND_COUNT],
    pending: [Option<VisualAnimation>; TARGET_KIND_COUNT],
    retained: Vec<VisualAnimation>,
}

impl CompositorAnimationEffectState {
    fn resolved_values_for_target(&mut self, request: &Request, host: &Host) -> &KeyframeValueCache {
        let key = request.cache_key();
        let slot = &mut self.keyframe_value_caches[target_kind_index(request.target_kind())];
        if !slot.as_ref().is_some_and(|cache| cache.key == key) {
            *slot = Some(build_keyframe_value_cache(request, host, key));
        }
        slot.as_ref().expect("the cache was just filled")
    }

    /// Builds the animation of the request's target kind and keeps it pending. `target_node_indices`
    /// names the target's nodes of the kind the animation drives.
    pub fn build(
        &mut self,
        request: &Request,
        host: &Host,
        target_node_indices: impl FnOnce(FfiVisualAnimationTargetKind) -> Vec<u32>,
    ) -> FfiCompositorAnimationBuildOutcome {
        let mut outcome = FfiCompositorAnimationBuildOutcome::default();
        let kind = request.target_kind();
        if request
            .keyframes
            .iter()
            .any(|keyframe| !keyframe.composite_is_replace || !keyframe.easing_is_supported)
        {
            return outcome;
        }

        let Some(values) = self.resolved_values_for_target(request, host).values.as_ref() else {
            return outcome;
        };
        debug_assert_eq!(values.len(), request.keyframes.len());
        let keyframes: Vec<VisualAnimationKeyframe> = request
            .keyframes
            .iter()
            .zip(values)
            .filter_map(|(keyframe, value)| {
                value.as_ref().map(|value| VisualAnimationKeyframe {
                    offset: keyframe.offset,
                    // SAFETY: The request's easing point ranges are live for the build.
                    easing: unsafe { Easing::from_descriptor(&keyframe.easing) },
                    value: value.clone(),
                })
            })
            .collect();

        if kind == FfiVisualAnimationTargetKind::Transform {
            outcome.only_translates_horizontally_is_known = true;
            outcome.only_translates_horizontally = request.targets_only_transform()
                && transform_lists_only_translate_horizontally(keyframes.iter().filter_map(|keyframe| {
                    match &keyframe.value {
                        VisualAnimationValue::Transform(operations) => Some(operations.as_slice()),
                        _ => None,
                    }
                }));
        }

        let timing = request.timing();
        let animation = VisualAnimation {
            target_kind: kind,
            node_indices: target_node_indices(kind),
            monotonic_time_at_anchor_ns: timing.monotonic_time_at_anchor_ns,
            local_time_at_anchor_ms: timing.local_time_at_anchor_ms,
            playback_rate: timing.playback_rate,
            start_delay_ms: timing.start_delay_ms,
            iteration_duration_ms: timing.iteration_duration_ms,
            iteration_count: timing.iteration_count,
            iteration_start: timing.iteration_start,
            playback_direction: timing.playback_direction,
            fill_mode: timing.fill_mode,
            // SAFETY: The request's easing point ranges are live for the build.
            easing: unsafe { Easing::from_descriptor(&timing.easing) },
            keyframes,
        };
        // An unsupported animation is turned down before a missing target node is asked for, so that
        // it cannot force and release that node on every attempt.
        if !animation.has_valid_parameters() {
            return outcome;
        }
        if animation.node_indices.is_empty() {
            outcome.missing_visual_context_node = true;
            return outcome;
        }
        self.pending[target_kind_index(kind)] = Some(animation);
        outcome.built = true;
        outcome
    }

    pub fn discard_pending(&mut self, kind: FfiVisualAnimationTargetKind) {
        self.pending[target_kind_index(kind)] = None;
    }

    pub fn has_pending(&self) -> bool {
        self.pending.iter().any(Option::is_some)
    }

    pub fn clear_pending(&mut self) {
        self.pending = Default::default();
    }

    /// Takes the pending animations as the effect's published ones and hands out copies for the
    /// document's list. An effect published before keeps the anchors of the animations whose
    /// parameters did not change, so that a rendering update advancing the main thread's timeline
    /// does not restart compositor playback.
    pub fn publish_pending(&mut self, reuse_retained_timing_anchors: bool) -> Vec<VisualAnimation> {
        let mut animations: Vec<VisualAnimation> = self.pending.iter_mut().filter_map(Option::take).collect();
        if reuse_retained_timing_anchors {
            for animation in &mut animations {
                if let Some(retained) = self
                    .retained
                    .iter()
                    .find(|retained| animation.has_same_animation_parameters(retained))
                {
                    animation.monotonic_time_at_anchor_ns = retained.monotonic_time_at_anchor_ns;
                    animation.local_time_at_anchor_ms = retained.local_time_at_anchor_ms;
                }
            }
        }
        self.retained = animations.clone();
        animations
    }

    pub fn has_retained(&self) -> bool {
        !self.retained.is_empty()
    }

    pub fn clear_retained(&mut self) {
        self.retained.clear();
    }

    /// Forgets everything: the keyframes changed.
    pub fn reset(&mut self) {
        self.keyframe_value_caches = Default::default();
        self.clear_pending();
        self.clear_retained();
    }
}

/// # Safety
///
/// `state` must be a live handle from `compositor_animation_effect_state_create`.
pub(crate) unsafe fn effect_state_from_handle<'a>(state: *mut c_void) -> &'a mut CompositorAnimationEffectState {
    // SAFETY: The caller guarantees a live, exclusively used handle.
    unsafe { &mut *state.cast::<CompositorAnimationEffectState>() }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::easing::{FfiEasingDescriptor, FfiEasingKind, FfiLinearEasingPoint};
    use crate::css::style_value::{ColorBase, CssString, RetainedStyleValueDataList};
    use crate::css::table_group_builder::angle_unit_index;
    use crate::painting::host::{FfiVisualAnimationFillMode, FfiVisualAnimationPlaybackDirection};
    use crate::painting::visual_context::publish_compositor_animations;
    use crate::painting::visual_context::{TransformData, TransformDataRole, VisualContextState, VisualContextTree};
    use FfiVisualAnimationTransformOperationKind as Kind;
    use libgfx_rust::{FloatMatrix4x4, FloatPoint};
    use std::cell::RefCell;
    use std::collections::HashMap;
    use std::rc::Rc;
    use std::sync::Arc;

    fn retained(value: StyleValueData) -> RetainedStyleValueData {
        RetainedStyleValueData::from_owned(value)
    }

    fn px(value: f64) -> StyleValueData {
        StyleValueData::Length {
            value,
            unit: crate::css::style_compute::px_length_unit(),
        }
    }

    fn deg(value: f64) -> StyleValueData {
        StyleValueData::Angle {
            value,
            unit: angle_unit_index("deg") as u8,
        }
    }

    fn number_value(value: f64) -> StyleValueData {
        StyleValueData::Number { value }
    }

    fn percentage(value: f64) -> StyleValueData {
        StyleValueData::Percentage { value }
    }

    fn none() -> StyleValueData {
        StyleValueData::Keyword { keyword: keyword::NONE }
    }

    fn opacity_value(value: f64) -> StyleValueData {
        StyleValueData::OpacityValue {
            value: retained(number_value(value)),
        }
    }

    fn transformation(function: u8, values: Vec<StyleValueData>) -> StyleValueData {
        StyleValueData::Transformation {
            property: property_id::TRANSFORM,
            transform_function: function,
            values: RetainedStyleValueDataList::from_retained_values(values.into_iter().map(retained).collect()),
        }
    }

    fn value_list(values: Vec<StyleValueData>) -> StyleValueData {
        StyleValueData::ValueList {
            values: RetainedStyleValueDataList::from_retained_values(values.into_iter().map(retained).collect()),
            separator: 0,
            collapsible: false,
        }
    }

    fn color_function(syntax: u8) -> StyleValueData {
        StyleValueData::ColorFunction {
            color_base: ColorBase {
                has_color_type: false,
                color_type: 0,
                color_syntax: syntax,
            },
            channel_0: retained(number_value(255.0)),
            channel_1: retained(number_value(0.0)),
            channel_2: retained(number_value(0.0)),
            alpha: retained(number_value(1.0)),
            has_name: false,
            name: CssString::default(),
            origin_color: RetainedStyleValueData::none(),
        }
    }

    fn filter(kind: u8, color_operation: u8, value: StyleValueData) -> StyleValueData {
        StyleValueData::Filter {
            kind,
            color_operation,
            value: retained(value),
        }
    }

    fn shadow(color: Option<StyleValueData>, offset_x: f64, offset_y: f64, blur_radius: Option<f64>) -> StyleValueData {
        StyleValueData::Shadow {
            shadow_type: 0,
            color: color.map_or_else(RetainedStyleValueData::none, retained),
            offset_x: retained(px(offset_x)),
            offset_y: retained(px(offset_y)),
            blur_radius: blur_radius.map_or_else(RetainedStyleValueData::none, |radius| retained(px(radius))),
            spread_distance: RetainedStyleValueData::none(),
            placement: 0,
        }
    }

    /// A main thread whose keyframe values are given up front, counting what the builder asks for.
    #[derive(Default)]
    struct TestHost {
        values: HashMap<(usize, u16, bool), Arc<StyleValueData>>,
        resolved_color: Option<Color>,
        resolutions: RefCell<usize>,
    }

    impl TestHost {
        fn with_value(mut self, keyframe_index: usize, property: AnimatedProperty, value: StyleValueData) -> Self {
            self.values
                .insert((keyframe_index, property.property_id(), false), Arc::new(value));
            self
        }

        fn with_underlying_value(
            mut self,
            keyframe_index: usize,
            property: AnimatedProperty,
            value: StyleValueData,
        ) -> Self {
            self.values
                .insert((keyframe_index, property.property_id(), true), Arc::new(value));
            self
        }

        fn ffi(&self) -> FfiCompositorAnimationHost {
            FfiCompositorAnimationHost {
                context: std::ptr::from_ref(self).cast_mut().cast(),
                resolved_keyframe_value: test_resolved_keyframe_value,
                resolve_color: test_resolve_color,
            }
        }
    }

    unsafe extern "C" fn test_resolved_keyframe_value(
        context: *mut c_void,
        keyframe_index: usize,
        property_id: u16,
        uses_underlying_style: bool,
    ) -> *const c_void {
        let host = unsafe { &*context.cast::<TestHost>() };
        *host.resolutions.borrow_mut() += 1;
        host.values
            .get(&(keyframe_index, property_id, uses_underlying_style))
            .map_or(std::ptr::null(), |value| Arc::into_raw(Arc::clone(value)).cast())
    }

    unsafe extern "C" fn test_resolve_color(context: *mut c_void, _value: *const c_void, color: *mut Color) -> bool {
        let host = unsafe { &*context.cast::<TestHost>() };
        match host.resolved_color {
            Some(resolved) => {
                unsafe { *color = resolved };
                true
            }
            None => false,
        }
    }

    fn linear_easing() -> FfiEasingDescriptor {
        static POINTS: [FfiLinearEasingPoint; 2] = [
            FfiLinearEasingPoint {
                input: 0.0,
                output: 0.0,
            },
            FfiLinearEasingPoint {
                input: 1.0,
                output: 1.0,
            },
        ];
        FfiEasingDescriptor {
            kind: FfiEasingKind::Linear,
            linear_points: POINTS.as_ptr(),
            linear_point_count: POINTS.len(),
            x1: 0.0,
            y1: 0.0,
            x2: 1.0,
            y2: 1.0,
            interval_count: 1,
            step_position: 0,
        }
    }

    fn keyframe(
        offset: f64,
        property: AnimatedProperty,
        state: FfiCompositorKeyframeValueState,
    ) -> FfiCompositorAnimationKeyframe {
        let absent = FfiCompositorKeyframeValueState::Absent;
        let mut keyframe = FfiCompositorAnimationKeyframe {
            offset,
            easing: linear_easing(),
            easing_is_supported: true,
            composite_is_replace: true,
            opacity: absent,
            background_color: absent,
            filter: absent,
            translate: absent,
            rotate: absent,
            scale: absent,
            transform: absent,
        };
        match property {
            AnimatedProperty::Opacity => keyframe.opacity = state,
            AnimatedProperty::BackgroundColor => keyframe.background_color = state,
            AnimatedProperty::Filter => keyframe.filter = state,
            AnimatedProperty::Translate => keyframe.translate = state,
            AnimatedProperty::Rotate => keyframe.rotate = state,
            AnimatedProperty::Scale => keyframe.scale = state,
            AnimatedProperty::Transform => keyframe.transform = state,
        }
        keyframe
    }

    fn timing() -> crate::painting::host::FfiCompositorAnimationTiming {
        crate::painting::host::FfiCompositorAnimationTiming {
            monotonic_time_at_anchor_ns: 1_000,
            local_time_at_anchor_ms: 0.0,
            playback_rate: 1.0,
            start_delay_ms: 0.0,
            iteration_duration_ms: 1000.0,
            iteration_count: f64::INFINITY,
            iteration_start: 0.0,
            playback_direction: FfiVisualAnimationPlaybackDirection::Normal,
            fill_mode: FfiVisualAnimationFillMode::None,
            easing: linear_easing(),
        }
    }

    fn build_request(
        kind: FfiVisualAnimationTargetKind,
        keyframes: &[FfiCompositorAnimationKeyframe],
        targeted_transform_properties: u8,
    ) -> FfiCompositorAnimationRequest {
        FfiCompositorAnimationRequest {
            target_kind: kind,
            layout_node: crate::layout::node_data::NodeSlotId::INVALID,
            timing: timing(),
            keyframes: keyframes.as_ptr(),
            keyframe_count: keyframes.len(),
            key_frame_set_identity: 7,
            target_style_generation: 1,
            style_environment_version: 1,
            reference_box_width: 200.0,
            reference_box_height: 100.0,
            device_pixels_per_css_pixel: 2.0,
            targeted_transform_properties,
            targets_only_transform: targeted_transform_properties == TARGETED_TRANSFORM_PROPERTY_TRANSFORM,
        }
    }

    fn present(offset: f64, property: AnimatedProperty) -> FfiCompositorAnimationKeyframe {
        keyframe(offset, property, FfiCompositorKeyframeValueState::Present)
    }

    fn operation(kind: Kind, values: &[f32]) -> VisualAnimationTransformOperation {
        VisualAnimationTransformOperation::new(kind, values).unwrap()
    }

    fn pending_animation(
        state: &CompositorAnimationEffectState,
        kind: FfiVisualAnimationTargetKind,
    ) -> &VisualAnimation {
        state.pending[target_kind_index(kind)]
            .as_ref()
            .expect("the animation is pending")
    }

    #[test]
    fn opacity_keyframes_build_once_and_come_from_the_cache_after() {
        let host = TestHost::default()
            .with_value(0, AnimatedProperty::Opacity, opacity_value(0.25))
            .with_value(1, AnimatedProperty::Opacity, opacity_value(0.75));
        let ffi_host = host.ffi();
        let keyframes = [
            present(0.0, AnimatedProperty::Opacity),
            present(1.0, AnimatedProperty::Opacity),
        ];
        let ffi_request = build_request(FfiVisualAnimationTargetKind::Opacity, &keyframes, 0);
        let request = unsafe { Request::new(&ffi_request) };
        let mut state = CompositorAnimationEffectState::default();

        let outcome = state.build(&request, &Host::new(&ffi_host), |_| vec![3]);
        assert!(outcome.built);
        assert!(!outcome.missing_visual_context_node);
        assert!(!outcome.only_translates_horizontally_is_known);
        let animation = pending_animation(&state, FfiVisualAnimationTargetKind::Opacity);
        assert_eq!(animation.node_indices, vec![3]);
        assert_eq!(animation.monotonic_time_at_anchor_ns, 1_000);
        assert_eq!(animation.keyframes.len(), 2);
        assert_eq!(animation.keyframes[0].value, VisualAnimationValue::Opacity(0.25));
        assert_eq!(animation.keyframes[1].offset, 1.0);
        assert_eq!(animation.keyframes[1].value, VisualAnimationValue::Opacity(0.75));
        assert!(animation.is_valid());
        assert_eq!(*host.resolutions.borrow(), 2);

        let outcome = state.build(&request, &Host::new(&ffi_host), |_| vec![3]);
        assert!(outcome.built);
        assert_eq!(*host.resolutions.borrow(), 2, "the cached values are reused");

        let mut changed_style = ffi_request;
        changed_style.target_style_generation += 1;
        let request = unsafe { Request::new(&changed_style) };
        assert!(state.build(&request, &Host::new(&ffi_host), |_| vec![3]).built);
        assert_eq!(*host.resolutions.borrow(), 4, "a style change lowers the values again");
    }

    #[test]
    fn a_missing_target_node_and_an_unsupported_keyframe_are_reported() {
        let host = TestHost::default()
            .with_value(0, AnimatedProperty::Opacity, opacity_value(0.0))
            .with_value(1, AnimatedProperty::Opacity, opacity_value(1.0));
        let ffi_host = host.ffi();
        let keyframes = [
            present(0.0, AnimatedProperty::Opacity),
            present(1.0, AnimatedProperty::Opacity),
        ];
        let ffi_request = build_request(FfiVisualAnimationTargetKind::Opacity, &keyframes, 0);
        let request = unsafe { Request::new(&ffi_request) };
        let mut state = CompositorAnimationEffectState::default();

        let outcome = state.build(&request, &Host::new(&ffi_host), |_| Vec::new());
        assert!(!outcome.built);
        assert!(outcome.missing_visual_context_node);
        assert!(!state.has_pending());

        let mut additive = keyframes;
        additive[1].composite_is_replace = false;
        let ffi_request = build_request(FfiVisualAnimationTargetKind::Opacity, &additive, 0);
        let request = unsafe { Request::new(&ffi_request) };
        assert!(!state.build(&request, &Host::new(&ffi_host), |_| vec![3]).built);

        let mut unsupported_easing = keyframes;
        unsupported_easing[0].easing_is_supported = false;
        let ffi_request = build_request(FfiVisualAnimationTargetKind::Opacity, &unsupported_easing, 0);
        let request = unsafe { Request::new(&ffi_request) };
        assert!(!state.build(&request, &Host::new(&ffi_host), |_| vec![3]).built);

        // A keyframe without the property is left out; too few remain for an animation. Another
        // keyframe set has an identity of its own.
        let sparse = [
            present(0.0, AnimatedProperty::Opacity),
            keyframe(1.0, AnimatedProperty::Opacity, FfiCompositorKeyframeValueState::Absent),
        ];
        let mut ffi_request = build_request(FfiVisualAnimationTargetKind::Opacity, &sparse, 0);
        ffi_request.key_frame_set_identity += 1;
        let request = unsafe { Request::new(&ffi_request) };
        assert!(!state.build(&request, &Host::new(&ffi_host), |_| vec![3]).built);
    }

    #[test]
    fn transform_keyframes_lower_to_operations_in_device_pixels_and_radians() {
        let host = TestHost::default()
            .with_value(
                0,
                AnimatedProperty::Transform,
                value_list(vec![
                    transformation(transform_function::TRANSLATE, vec![percentage(10.0), px(5.0)]),
                    transformation(transform_function::ROTATE, vec![deg(90.0)]),
                ]),
            )
            .with_value(
                1,
                AnimatedProperty::Transform,
                value_list(vec![
                    transformation(transform_function::TRANSLATE, vec![px(20.0), px(0.0)]),
                    transformation(transform_function::ROTATE, vec![deg(180.0)]),
                ]),
            );
        let ffi_host = host.ffi();
        let keyframes = [
            present(0.0, AnimatedProperty::Transform),
            present(1.0, AnimatedProperty::Transform),
        ];
        let ffi_request = build_request(
            FfiVisualAnimationTargetKind::Transform,
            &keyframes,
            TARGETED_TRANSFORM_PROPERTY_TRANSFORM,
        );
        let request = unsafe { Request::new(&ffi_request) };
        let mut state = CompositorAnimationEffectState::default();

        let outcome = state.build(&request, &Host::new(&ffi_host), |_| vec![1]);
        assert!(outcome.built);
        assert!(outcome.only_translates_horizontally_is_known);
        assert!(!outcome.only_translates_horizontally);
        let animation = pending_animation(&state, FfiVisualAnimationTargetKind::Transform);
        assert_eq!(
            animation.keyframes[0].value,
            VisualAnimationValue::Transform(vec![
                operation(Kind::Translate, &[40.0, 10.0]),
                operation(Kind::Rotate, &[std::f32::consts::FRAC_PI_2]),
            ])
        );
        assert_eq!(
            animation.keyframes[1].value,
            VisualAnimationValue::Transform(vec![
                operation(Kind::Translate, &[40.0, 0.0]),
                operation(Kind::Rotate, &[std::f32::consts::PI]),
            ])
        );
        assert!(animation.is_valid());

        // A single translation becomes the horizontal one, and a rotation about one axis the
        // rotation about that axis, signed by the axis.
        let lowered = transform_operations_from_style_value(
            AnimatedProperty::Transform,
            &value_list(vec![
                transformation(transform_function::TRANSLATE_X, vec![px(1.5)]),
                transformation(transform_function::TRANSLATE, vec![percentage(50.0)]),
                transformation(
                    transform_function::ROTATE3D,
                    vec![number_value(0.0), number_value(0.0), number_value(-1.0), deg(90.0)],
                ),
            ]),
            (200.0, 100.0),
            2.0,
            None,
        );
        assert_eq!(
            lowered,
            Some(vec![
                operation(Kind::TranslateX, &[3.0]),
                operation(Kind::TranslateX, &[200.0]),
                operation(Kind::RotateZ, &[-std::f32::consts::FRAC_PI_2]),
            ])
        );
        let tilted = transform_operations_from_style_value(
            AnimatedProperty::Transform,
            &value_list(vec![transformation(
                transform_function::ROTATE3D,
                vec![number_value(1.0), number_value(1.0), number_value(0.0), deg(90.0)],
            )]),
            (200.0, 100.0),
            2.0,
            None,
        );
        assert_eq!(tilted, None);
        let matrix = transform_operations_from_style_value(
            AnimatedProperty::Transform,
            &value_list(vec![transformation(
                transform_function::MATRIX,
                vec![number_value(1.0); 6],
            )]),
            (200.0, 100.0),
            2.0,
            None,
        );
        assert_eq!(matrix, None);
    }

    #[test]
    fn a_none_transform_takes_the_shape_of_the_other_keyframes() {
        let host = TestHost::default()
            .with_value(0, AnimatedProperty::Transform, none())
            .with_value(
                1,
                AnimatedProperty::Transform,
                value_list(vec![
                    transformation(transform_function::SCALE, vec![number_value(2.0)]),
                    transformation(transform_function::TRANSLATE_Y, vec![px(4.0)]),
                ]),
            )
            .with_underlying_value(0, AnimatedProperty::Scale, none())
            .with_value(
                1,
                AnimatedProperty::Scale,
                transformation(transform_function::SCALE, vec![number_value(3.0)]),
            );
        let ffi_host = host.ffi();

        let keyframes = [
            present(0.0, AnimatedProperty::Transform),
            present(1.0, AnimatedProperty::Transform),
        ];
        let ffi_request = build_request(
            FfiVisualAnimationTargetKind::Transform,
            &keyframes,
            TARGETED_TRANSFORM_PROPERTY_TRANSFORM,
        );
        let request = unsafe { Request::new(&ffi_request) };
        let mut state = CompositorAnimationEffectState::default();
        assert!(state.build(&request, &Host::new(&ffi_host), |_| vec![1]).built);
        let animation = pending_animation(&state, FfiVisualAnimationTargetKind::Transform);
        assert_eq!(
            animation.keyframes[0].value,
            VisualAnimationValue::Transform(vec![
                operation(Kind::Scale, &[1.0]),
                operation(Kind::TranslateY, &[0.0])
            ])
        );
        assert!(animation.is_valid());

        // The scale property alone: a keyframe that takes the underlying style, which is none, gets
        // the property's own identity when no other keyframe offers a shape... except the second
        // keyframe does, so that shape is taken.
        let keyframes = [
            keyframe(
                0.0,
                AnimatedProperty::Scale,
                FfiCompositorKeyframeValueState::UsesUnderlyingStyle,
            ),
            present(1.0, AnimatedProperty::Scale),
        ];
        let ffi_request = build_request(
            FfiVisualAnimationTargetKind::Transform,
            &keyframes,
            TARGETED_TRANSFORM_PROPERTY_SCALE,
        );
        let request = unsafe { Request::new(&ffi_request) };
        let mut state = CompositorAnimationEffectState::default();
        assert!(state.build(&request, &Host::new(&ffi_host), |_| vec![1]).built);
        let animation = pending_animation(&state, FfiVisualAnimationTargetKind::Transform);
        assert_eq!(
            animation.keyframes[0].value,
            VisualAnimationValue::Transform(vec![operation(Kind::Scale, &[1.0])])
        );
        assert_eq!(
            animation.keyframes[1].value,
            VisualAnimationValue::Transform(vec![operation(Kind::Scale, &[3.0])])
        );
    }

    #[test]
    fn transform_keyframes_that_only_translate_horizontally_are_told_apart() {
        let host = TestHost::default()
            .with_value(
                0,
                AnimatedProperty::Transform,
                value_list(vec![transformation(
                    transform_function::TRANSLATE,
                    vec![px(0.0), px(3.0)],
                )]),
            )
            .with_value(
                1,
                AnimatedProperty::Transform,
                value_list(vec![transformation(
                    transform_function::TRANSLATE,
                    vec![px(50.0), px(3.0)],
                )]),
            );
        let ffi_host = host.ffi();
        let keyframes = [
            present(0.0, AnimatedProperty::Transform),
            present(1.0, AnimatedProperty::Transform),
        ];
        let ffi_request = build_request(
            FfiVisualAnimationTargetKind::Transform,
            &keyframes,
            TARGETED_TRANSFORM_PROPERTY_TRANSFORM,
        );
        let request = unsafe { Request::new(&ffi_request) };
        assert!(effect_only_translates_horizontally(&request, &Host::new(&ffi_host)));
        assert!(effect_transform_preserves_axes(&request, &Host::new(&ffi_host)));

        let mut state = CompositorAnimationEffectState::default();
        let outcome = state.build(&request, &Host::new(&ffi_host), |_| vec![1]);
        assert!(outcome.built);
        assert!(outcome.only_translates_horizontally);

        let mut with_rotate = ffi_request;
        with_rotate.targeted_transform_properties |= TARGETED_TRANSFORM_PROPERTY_ROTATE;
        with_rotate.targets_only_transform = false;
        let request = unsafe { Request::new(&with_rotate) };
        assert!(!effect_only_translates_horizontally(&request, &Host::new(&ffi_host)));
        assert!(!effect_transform_preserves_axes(&request, &Host::new(&ffi_host)));

        let vertical = TestHost::default()
            .with_value(
                0,
                AnimatedProperty::Transform,
                value_list(vec![transformation(
                    transform_function::TRANSLATE,
                    vec![px(0.0), px(3.0)],
                )]),
            )
            .with_value(
                1,
                AnimatedProperty::Transform,
                value_list(vec![transformation(
                    transform_function::TRANSLATE,
                    vec![px(50.0), px(4.0)],
                )]),
            );
        let vertical_host = vertical.ffi();
        let request = unsafe { Request::new(&ffi_request) };
        assert!(!effect_only_translates_horizontally(
            &request,
            &Host::new(&vertical_host)
        ));
        assert!(effect_transform_preserves_axes(&request, &Host::new(&vertical_host)));
    }

    #[test]
    fn filter_keyframes_lower_to_filter_functions() {
        let host = TestHost {
            resolved_color: Some(Color::from_rgb(255, 0, 0)),
            ..TestHost::default()
        }
        .with_value(0, AnimatedProperty::Filter, none())
        .with_value(
            1,
            AnimatedProperty::Filter,
            value_list(vec![
                filter(FILTER_KIND_BLUR, 0, px(4.0)),
                filter(
                    FILTER_KIND_DROP_SHADOW,
                    0,
                    shadow(Some(color_function(COLOR_SYNTAX_LEGACY)), 1.0, 2.0, Some(3.0)),
                ),
                filter(FILTER_KIND_HUE_ROTATE, 0, deg(90.0)),
                filter(FILTER_KIND_COLOR, ColorFilterType::Sepia as u8, percentage(50.0)),
            ]),
        );
        let ffi_host = host.ffi();
        let keyframes = [
            present(0.0, AnimatedProperty::Filter),
            present(1.0, AnimatedProperty::Filter),
        ];
        let ffi_request = build_request(FfiVisualAnimationTargetKind::Filter, &keyframes, 0);
        let request = unsafe { Request::new(&ffi_request) };
        let mut state = CompositorAnimationEffectState::default();
        assert!(state.build(&request, &Host::new(&ffi_host), |_| vec![2]).built);
        let animation = pending_animation(&state, FfiVisualAnimationTargetKind::Filter);
        assert_eq!(animation.keyframes[0].value, VisualAnimationValue::Filter(Vec::new()));
        let VisualAnimationValue::Filter(functions) = &animation.keyframes[1].value else {
            panic!("not a filter value");
        };
        assert_eq!(functions.len(), 4);
        assert_eq!(functions[0].kind, FfiFilterFunctionKind::Blur);
        assert_eq!(functions[0].amount, 8.0);
        assert_eq!(functions[1].kind, FfiFilterFunctionKind::DropShadow);
        assert_eq!(
            (functions[1].offset_x, functions[1].offset_y, functions[1].amount),
            (2.0, 4.0, 6.0)
        );
        assert_eq!(functions[1].color, Color::from_rgb(255, 0, 0));
        assert_eq!(functions[2].kind, FfiFilterFunctionKind::HueRotate);
        assert_eq!(functions[2].amount, 90.0);
        assert_eq!(functions[3].kind, FfiFilterFunctionKind::Color);
        assert_eq!(functions[3].color_operation, ColorFilterType::Sepia);
        assert_eq!(functions[3].amount, 0.5);
        assert!(animation.is_valid());

        // A drop shadow in a modern color syntax keeps the effect on the main thread.
        let modern = TestHost {
            resolved_color: Some(Color::from_rgb(255, 0, 0)),
            ..TestHost::default()
        }
        .with_value(0, AnimatedProperty::Filter, none())
        .with_value(
            1,
            AnimatedProperty::Filter,
            value_list(vec![filter(
                FILTER_KIND_DROP_SHADOW,
                0,
                shadow(Some(color_function(1)), 1.0, 2.0, None),
            )]),
        );
        let modern_host = modern.ffi();
        let mut state = CompositorAnimationEffectState::default();
        assert!(!state.build(&request, &Host::new(&modern_host), |_| vec![2]).built);
    }

    #[test]
    fn background_colors_keep_modern_syntaxes_and_currentcolor_on_the_main_thread() {
        let build = |value: StyleValueData| {
            let host = TestHost {
                resolved_color: Some(Color::from_rgb(0, 0, 255)),
                ..TestHost::default()
            }
            .with_value(
                0,
                AnimatedProperty::BackgroundColor,
                color_function(COLOR_SYNTAX_LEGACY),
            )
            .with_value(1, AnimatedProperty::BackgroundColor, value);
            let ffi_host = host.ffi();
            let keyframes = [
                present(0.0, AnimatedProperty::BackgroundColor),
                present(1.0, AnimatedProperty::BackgroundColor),
            ];
            let ffi_request = build_request(FfiVisualAnimationTargetKind::BackgroundColor, &keyframes, 0);
            let request = unsafe { Request::new(&ffi_request) };
            let mut state = CompositorAnimationEffectState::default();
            let built = state.build(&request, &Host::new(&ffi_host), |_| vec![2]).built;
            built.then(|| pending_animation(&state, FfiVisualAnimationTargetKind::BackgroundColor).clone())
        };
        let legacy = build(color_function(COLOR_SYNTAX_LEGACY)).expect("legacy colors are compositor driven");
        assert_eq!(
            legacy.keyframes[1].value,
            VisualAnimationValue::BackgroundColor(Color::from_rgb(0, 0, 255))
        );
        assert!(build(color_function(1)).is_none());
        assert!(
            build(StyleValueData::Keyword {
                keyword: keyword::CURRENTCOLOR
            })
            .is_none()
        );
        // A keyword the main thread can resolve, such as a system color, is taken.
        assert!(build(StyleValueData::Keyword { keyword: keyword::NONE }).is_some());
    }

    fn tree_with_one_effect() -> Rc<VisualContextTree> {
        Rc::new(VisualContextTree::create(TransformData {
            matrix: FloatMatrix4x4::identity(),
            origin: FloatPoint { x: 0.0, y: 0.0 },
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
        }))
    }

    fn built_opacity_animation(anchor_ns: i64) -> VisualAnimation {
        VisualAnimation {
            node_indices: vec![0],
            monotonic_time_at_anchor_ns: anchor_ns,
            iteration_duration_ms: 1000.0,
            keyframes: vec![
                VisualAnimationKeyframe {
                    offset: 0.0,
                    easing: Easing::default(),
                    value: VisualAnimationValue::Opacity(0.0),
                },
                VisualAnimationKeyframe {
                    offset: 1.0,
                    easing: Easing::default(),
                    value: VisualAnimationValue::Opacity(1.0),
                },
            ],
            ..VisualAnimation::default()
        }
    }

    #[test]
    fn publishing_keeps_the_anchor_of_an_unchanged_animation_and_reports_what_changed() {
        let mut state = CompositorAnimationEffectState::default();
        state.pending[target_kind_index(FfiVisualAnimationTargetKind::Opacity)] = Some(built_opacity_animation(100));
        let first = state.publish_pending(false);
        assert_eq!(first.len(), 1);
        assert!(state.has_retained());
        assert!(!state.has_pending());

        // The next pass built the same animation anchored later; the published anchor stays.
        state.pending[target_kind_index(FfiVisualAnimationTargetKind::Opacity)] = Some(built_opacity_animation(200));
        let again = state.publish_pending(true);
        assert_eq!(again[0].monotonic_time_at_anchor_ns, 100);

        let mut retimed = built_opacity_animation(300);
        retimed.iteration_duration_ms = 500.0;
        state.pending[target_kind_index(FfiVisualAnimationTargetKind::Opacity)] = Some(retimed);
        let changed = state.publish_pending(true);
        assert_eq!(changed[0].monotonic_time_at_anchor_ns, 300);

        state.clear_retained();
        assert!(!state.has_retained());

        let mut visual_context = VisualContextState {
            tree: Some(tree_with_one_effect()),
            ..VisualContextState::default()
        };
        assert!(!publish_compositor_animations(&mut visual_context, true).published);

        visual_context.pending_compositor_animations = first.clone();
        let outcome = publish_compositor_animations(&mut visual_context, true);
        assert!(outcome.published);
        assert!(outcome.parameters_changed);
        assert!(!outcome.timing_anchors_changed);
        assert!(visual_context.tree.as_ref().unwrap().has_visual_animations());

        visual_context.pending_compositor_animations = first.clone();
        assert!(!publish_compositor_animations(&mut visual_context, true).published);

        visual_context.pending_compositor_animations = again.clone();
        visual_context.pending_compositor_animations[0].monotonic_time_at_anchor_ns = 500;
        let outcome = publish_compositor_animations(&mut visual_context, true);
        assert!(outcome.published);
        assert!(!outcome.parameters_changed);
        assert!(outcome.timing_anchors_changed);

        visual_context.pending_compositor_animations = changed;
        let outcome = publish_compositor_animations(&mut visual_context, false);
        assert!(outcome.published);
        assert!(outcome.parameters_changed);
        assert!(!visual_context.tree.as_ref().unwrap().has_visual_animations());
        assert!(visual_context.pending_compositor_animations.is_empty());
    }
}
