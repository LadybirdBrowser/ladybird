/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Builds the compositor animations of a keyframe effect from its resolved keyframes: the lowering
//! of style values into the values the compositor interpolates, the per-effect cache of those
//! values, the animations an effect has built and published, and the document-wide list the visual
//! context tree receives.

use crate::animations::keyframes::{
    CompositeOperationOrAuto, Keyframe, KeyframeEasing, KeyframeSet, KeyframeValue, offset_of_key,
};
use crate::css::absolutize::number_from_value;
use crate::css::color_resolution::{ColorResolutionInput, FfiColorResolutionStyle, to_color};
use crate::css::css_enums::keyword;
use crate::css::css_pixels::CssPixels;
use crate::css::easing::Easing;
use crate::css::property_metadata::property_id;
use crate::css::serialize::{TRANSFORM_FUNCTION_PARAMETER_TYPES, transform_function};
use crate::css::style_value::{
    FILTER_KIND_BLUR, FILTER_KIND_COLOR, FILTER_KIND_DROP_SHADOW, FILTER_KIND_HUE_ROTATE, RetainedStyleValueData,
    StyleValueData, is_none_keyword,
};
use crate::css::style_value::{ShadowPayload, TransformationPayload};
use crate::css::table_group_builder::{
    TRANSFORM_PARAMETER_ANGLE, TRANSFORM_PARAMETER_LENGTH, TRANSFORM_PARAMETER_LENGTH_NONE,
    TRANSFORM_PARAMETER_LENGTH_PERCENTAGE, TRANSFORM_PARAMETER_NUMBER, TRANSFORM_PARAMETER_NUMBER_PERCENTAGE,
    angle_degrees, angle_radians, length_px_unrounded,
};
use crate::css::value_codes::COLOR_SYNTAX_LEGACY;
use crate::painting::filter_bytes::{FfiFilterFunction, FfiFilterFunctionKind};
use crate::painting::host::{
    FfiCompositorAnimationBuildOutcome, FfiCompositorAnimationHost, FfiCompositorAnimationRequest,
    FfiCompositorAnimationTiming, FfiVisualAnimationTargetKind, FfiVisualAnimationTransformOperationKind,
    TARGETED_TRANSFORM_PROPERTY_ROTATE, TARGETED_TRANSFORM_PROPERTY_SCALE, TARGETED_TRANSFORM_PROPERTY_TRANSFORM,
    TARGETED_TRANSFORM_PROPERTY_TRANSLATE,
};
use crate::painting::visual_animation::{
    VisualAnimation, VisualAnimationKeyframe, VisualAnimationTransformOperation, VisualAnimationValue,
};
use libgfx_rust::{Color, ColorFilterType};
use std::sync::Arc;

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

    /// The value of `property` a keyframe gives, `value`, resolved for the target.
    fn resolved_keyframe_value(
        &self,
        property: AnimatedProperty,
        value: KeyframeValueOf<'_>,
    ) -> Option<RetainedStyleValueData> {
        let declared_value = match value {
            KeyframeValueOf::Absent => return None,
            KeyframeValueOf::UnderlyingStyle => std::ptr::null(),
            KeyframeValueOf::Declared(value) => value.pointer().cast(),
        };
        // SAFETY: The host returns a retained style value or null, synchronously. The declared value is the keyframe
        //         set's, which the request keeps live.
        let value = unsafe { (self.0.resolved_keyframe_value)(self.0.context, property.property_id(), declared_value) };
        if value.is_null() {
            return None;
        }
        // SAFETY: The host hands over one strong reference, which the wrapper releases.
        Some(unsafe { RetainedStyleValueData::from_retained_pointer(value.cast()) })
    }
}

/// What a keyframe gives a property: a value of its own, the target's underlying style, or nothing.
#[derive(Clone, Copy)]
enum KeyframeValueOf<'a> {
    Absent,
    UnderlyingStyle,
    Declared(&'a RetainedStyleValueData),
}

/// A build request with the keyframes the effect runs in reach.
pub struct Request<'a> {
    ffi: &'a FfiCompositorAnimationRequest,
    keyframe_set: Option<Arc<KeyframeSet>>,
    /// The translation in CSS pixels that the x and y of a <use> element apply after its transform.
    additional_translation: (f32, f32),
}

impl<'a> Request<'a> {
    /// # Safety
    ///
    /// The request's keyframe set must be null or one the host holds a reference to.
    pub unsafe fn new(ffi: &'a FfiCompositorAnimationRequest) -> Self {
        Self {
            ffi,
            // SAFETY: Guaranteed by the caller.
            keyframe_set: (!ffi.keyframe_set.is_null())
                .then(|| unsafe { KeyframeSet::retained_from_host(ffi.keyframe_set.cast()) }),
            additional_translation: (0.0, 0.0),
        }
    }

    pub fn set_additional_translation(&mut self, x: f32, y: f32) {
        self.additional_translation = (x, y);
    }

    /// A request for the keyframes of `keyframe_set`, for a unit test.
    #[cfg(test)]
    fn with_keyframe_set(ffi: &'a FfiCompositorAnimationRequest, keyframe_set: &Arc<KeyframeSet>) -> Self {
        Self {
            ffi,
            keyframe_set: Some(Arc::clone(keyframe_set)),
            additional_translation: (0.0, 0.0),
        }
    }

    /// The keyframes the effect runs, in the order they are used.
    fn keyframes(&self) -> &[Keyframe] {
        self.keyframe_set.as_deref().map_or(&[], KeyframeSet::keyframes)
    }

    /// What `keyframe`, one of the effect's, gives `property`.
    fn value_of(&self, keyframe: &Keyframe, property: AnimatedProperty) -> KeyframeValueOf<'_> {
        let Some(keyframe_set) = &self.keyframe_set else {
            return KeyframeValueOf::Absent;
        };
        let property_id = property.property_id();
        match keyframe_set
            .declarations_of(keyframe)
            .iter()
            .find(|declaration| declaration.property_id == property_id)
            .map(|declaration| &declaration.value)
        {
            None => KeyframeValueOf::Absent,
            Some(KeyframeValue::ElementValue) => KeyframeValueOf::UnderlyingStyle,
            Some(KeyframeValue::Declared(value)) => KeyframeValueOf::Declared(value),
        }
    }

    /// The easing `keyframe`, one of the effect's, runs to the next one, or none for one still to be substituted
    /// against the target, which keeps the effect off the compositor.
    fn easing_of(&self, keyframe: &Keyframe) -> Option<Easing> {
        match &keyframe.easing {
            // SAFETY: The host keeps the default easing's points live for the build.
            KeyframeEasing::None => Some(unsafe { Easing::from_descriptor(&self.ffi.default_easing) }),
            KeyframeEasing::Easing(easing) => Some(easing.clone()),
            KeyframeEasing::Value(_) => None,
        }
    }

    /// Whether `keyframe`, one of the effect's, replaces the value beneath it.
    fn replaces(&self, keyframe: &Keyframe) -> bool {
        match keyframe.composite {
            CompositeOperationOrAuto::Replace => true,
            CompositeOperationOrAuto::Add | CompositeOperationOrAuto::Accumulate => false,
            CompositeOperationOrAuto::Auto => self.ffi.effect_composite_is_replace,
        }
    }

    pub fn target_kind(&self) -> FfiVisualAnimationTargetKind {
        self.ffi.target_kind
    }

    pub fn layout_node(&self) -> crate::layout::node_data::NodeSlotId {
        self.ffi.layout_node
    }

    /// The color a resolved color value names for the target.
    fn resolve_color(&self, value: &StyleValueData) -> Option<Color> {
        let target_style = FfiColorResolutionStyle {
            inherited_ui_values: self.ffi.target_inherited_ui_values,
            inherited_text_values: self.ffi.target_inherited_text_values,
        };
        // SAFETY: The host keeps the target's style group payloads live for the build.
        let input = unsafe { ColorResolutionInput::for_ffi_style(&raw const target_style) };
        let color = to_color(value, &input)?;
        Some(Color::from_rgba(color.r, color.g, color.b, color.a))
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
            keyframe_set: self.keyframe_set.clone(),
            target_style_generation: self.ffi.target_style_generation,
            style_environment_version: self.ffi.style_environment_version,
            reference_box_width: self.ffi.reference_box_width,
            reference_box_height: self.ffi.reference_box_height,
            device_pixels_per_css_pixel: self.ffi.device_pixels_per_css_pixel,
            additional_translation_x: self.additional_translation.0,
            additional_translation_y: self.additional_translation.1,
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
    matches!(value, StyleValueData::ColorFunction(color_function) if color_function.color_base.color_syntax == COLOR_SYNTAX_LEGACY)
}

/// Legacy sRGB colors interpolate in gamma-encoded sRGB, which the compositor sampler matches. Modern
/// color syntaxes stay on the main thread until compositor values can retain their interpolation
/// color space, and currentcolor with them.
fn background_color_from_style_value(value: &StyleValueData, request: &Request) -> Option<Color> {
    if matches!(value, StyleValueData::Keyword { keyword: code } if *code == keyword::CURRENTCOLOR) {
        return None;
    }
    if matches!(value, StyleValueData::ColorFunction { .. }) && !is_legacy_color_function(value) {
        return None;
    }
    request.resolve_color(value)
}

fn filter_functions_from_style_value(value: &StyleValueData, request: &Request) -> Option<Vec<FfiFilterFunction>> {
    let device_pixels_per_css_pixel = request.device_pixels_per_css_pixel();
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
                let StyleValueData::Shadow(shadow_payload) = value.data() else {
                    return None;
                };
                let ShadowPayload {
                    color,
                    offset_x,
                    offset_y,
                    blur_radius,
                    ..
                } = &**shadow_payload;
                // Gfx filters hold 8-bit sRGB colors. Modern color syntaxes and currentcolor stay on the
                // main thread until compositor filter values can retain their color space and syntax.
                let color = color.optional_data().filter(|color| is_legacy_color_function(color))?;
                let color = request.resolve_color(color)?;
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
    // https://drafts.csswg.org/css-transforms-2/#transform-primitives
    // Translations and scales take the operation of their primitive, through which the main thread interpolates two
    // functions that share it, so that keyframes naming different ones line up as the same operation.
    let primitive = match (kind, lowered.as_slice()) {
        (Kind::Translate | Kind::TranslateX, &[x]) => Some((Kind::Translate, vec![x, 0.0])),
        (Kind::TranslateY, &[y]) => Some((Kind::Translate, vec![0.0, y])),
        (Kind::TranslateZ, &[z]) => Some((Kind::Translate3d, vec![0.0, 0.0, z])),
        (Kind::Scale, &[scale]) => Some((Kind::Scale, vec![scale, scale])),
        (Kind::ScaleX, &[x]) => Some((Kind::Scale, vec![x, 1.0])),
        (Kind::ScaleY, &[y]) => Some((Kind::Scale, vec![1.0, y])),
        (Kind::ScaleZ, &[z]) => Some((Kind::Scale3d, vec![1.0, 1.0, z])),
        _ => None,
    };
    let (kind, lowered) = primitive.unwrap_or((kind, lowered));
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
        let StyleValueData::Transformation(transformation_payload) = transformation else {
            return None;
        };
        let TransformationPayload {
            transform_function,
            values,
            matrix_values,
            ..
        } = &**transformation_payload;
        // Matrix-only precision inputs must not replace unwrapped animation interpolation arguments.
        if !matrix_values.as_slice().is_empty() {
            return None;
        }
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
    if !first.iter().all(|operation| operation.kind == Kind::Translate) {
        return false;
    }
    let translate_y = |operation: &VisualAnimationTransformOperation| operation.values().get(1).copied().unwrap_or(0.0);
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
    for keyframe in request.keyframes() {
        let declared = match request.value_of(keyframe, AnimatedProperty::Transform) {
            KeyframeValueOf::Absent => continue,
            KeyframeValueOf::UnderlyingStyle => return None,
            declared @ KeyframeValueOf::Declared(_) => declared,
        };
        let value = host.resolved_keyframe_value(AnimatedProperty::Transform, declared)?;
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

#[derive(Clone)]
struct KeyframeValueCacheKey {
    keyframe_set: Option<Arc<KeyframeSet>>,
    target_style_generation: u64,
    style_environment_version: u64,
    reference_box_width: f32,
    reference_box_height: f32,
    device_pixels_per_css_pixel: f32,
    additional_translation_x: f32,
    additional_translation_y: f32,
}

impl PartialEq for KeyframeValueCacheKey {
    fn eq(&self, other: &Self) -> bool {
        let same_keyframe_set = match (&self.keyframe_set, &other.keyframe_set) {
            (Some(keyframe_set), Some(other_keyframe_set)) => Arc::ptr_eq(keyframe_set, other_keyframe_set),
            (None, None) => true,
            _ => false,
        };
        same_keyframe_set
            && self.target_style_generation == other.target_style_generation
            && self.style_environment_version == other.style_environment_version
            && self.reference_box_width == other.reference_box_width
            && self.reference_box_height == other.reference_box_height
            && self.device_pixels_per_css_pixel == other.device_pixels_per_css_pixel
            && self.additional_translation_x == other.additional_translation_x
            && self.additional_translation_y == other.additional_translation_y
    }
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
    let mut values: Option<Vec<Option<VisualAnimationValue>>> = request
        .keyframes()
        .iter()
        .map(|keyframe| lower_keyframe(request, host, keyframe, &mut identity_shapes))
        .collect();
    if let Some(values) = &mut values {
        give_each_transform_list_position_one_translation_and_scale_primitive(values);
    }
    KeyframeValueCache { key, values }
}

/// https://drafts.csswg.org/css-transforms-2/#interpolation-of-transform-functions
/// Where the keyframes name a 2D translation or scale at a position of their transform lists and a 3D one at the same
/// position, the two interpolate through their 3D primitive. The compositor interpolates operations of one kind, so
/// every keyframe takes the 2D operation where none of them leaves the plane, and the 3D one otherwise.
fn give_each_transform_list_position_one_translation_and_scale_primitive(values: &mut [Option<VisualAnimationValue>]) {
    use FfiVisualAnimationTransformOperationKind as Kind;
    const PRIMITIVES: [(Kind, Kind, f32); 2] = [
        (Kind::Translate, Kind::Translate3d, 0.0),
        (Kind::Scale, Kind::Scale3d, 1.0),
    ];
    let mut transform_lists: Vec<&mut Vec<VisualAnimationTransformOperation>> = values
        .iter_mut()
        .filter_map(|value| match value {
            Some(VisualAnimationValue::Transform(operations)) => Some(operations),
            _ => None,
        })
        .collect();
    let Some(position_count) = transform_lists.iter().map(|operations| operations.len()).min() else {
        return;
    };
    for position in 0..position_count {
        for (kind_in_plane, kind_in_space, identity) in PRIMITIVES {
            let some_keyframe_names = |kind| {
                transform_lists
                    .iter()
                    .any(|operations| operations[position].kind == kind)
            };
            let every_keyframe_names_one_of_the_two = transform_lists
                .iter()
                .all(|operations| [kind_in_plane, kind_in_space].contains(&operations[position].kind));
            if !every_keyframe_names_one_of_the_two
                || !some_keyframe_names(kind_in_plane)
                || !some_keyframe_names(kind_in_space)
            {
                continue;
            }
            let every_keyframe_stays_in_plane = transform_lists.iter().all(|operations| {
                let operation = &operations[position];
                operation.kind == kind_in_plane || operation.values().get(2) == Some(&identity)
            });
            let (kind_to_replace, kind_of_every_keyframe) = if every_keyframe_stays_in_plane {
                (kind_in_space, kind_in_plane)
            } else {
                (kind_in_plane, kind_in_space)
            };
            for operations in &mut transform_lists {
                let operation = &mut operations[position];
                if operation.kind != kind_to_replace {
                    continue;
                }
                let x = operation.values()[0];
                let y = operation.values().get(1).copied().unwrap_or(match operation.kind {
                    Kind::Scale => x,
                    _ => identity,
                });
                let values_of_every_keyframe: &[f32] = if every_keyframe_stays_in_plane {
                    &[x, y]
                } else {
                    &[x, y, identity]
                };
                if let Some(replacement) =
                    VisualAnimationTransformOperation::new(kind_of_every_keyframe, values_of_every_keyframe)
                {
                    *operation = replacement;
                }
            }
        }
    }
}

/// The value of the request's target kind one keyframe gives: none when the keyframe leaves the
/// kind to the effect to synthesize, and no outcome when the keyframe cannot be lowered.
fn lower_keyframe(
    request: &Request,
    host: &Host,
    keyframe: &Keyframe,
    identity_shapes: &mut TransformIdentityShapes,
) -> Option<Option<VisualAnimationValue>> {
    let kind = request.target_kind();
    let property = match kind {
        FfiVisualAnimationTargetKind::Opacity => AnimatedProperty::Opacity,
        FfiVisualAnimationTargetKind::BackgroundColor => AnimatedProperty::BackgroundColor,
        FfiVisualAnimationTargetKind::Filter => AnimatedProperty::Filter,
        FfiVisualAnimationTargetKind::Transform => {
            return lower_transform_keyframe(request, host, keyframe, identity_shapes);
        }
    };
    let value = request.value_of(keyframe, property);
    if matches!(value, KeyframeValueOf::Absent) {
        return Some(None);
    }
    let resolved = host.resolved_keyframe_value(property, value)?;
    let value = match kind {
        FfiVisualAnimationTargetKind::Opacity => {
            VisualAnimationValue::Opacity(opacity_from_style_value(resolved.data())?)
        }
        FfiVisualAnimationTargetKind::BackgroundColor => {
            VisualAnimationValue::BackgroundColor(background_color_from_style_value(resolved.data(), request)?)
        }
        _ => VisualAnimationValue::Filter(filter_functions_from_style_value(resolved.data(), request)?),
    };
    Some(Some(value))
}

fn lower_transform_keyframe(
    request: &Request,
    host: &Host,
    keyframe: &Keyframe,
    identity_shapes: &mut TransformIdentityShapes,
) -> Option<Option<VisualAnimationValue>> {
    let mut operations = Vec::new();
    for (property_index, property) in AnimatedProperty::TRANSFORM_FAMILY.iter().enumerate() {
        if !request.targets(*property) {
            continue;
        }
        let value = request.value_of(keyframe, *property);
        if matches!(value, KeyframeValueOf::Absent) {
            // A keyframe that leaves out the only transform property is synthesized by the effect;
            // one that leaves out some of several cannot be lowered.
            return (request.targeted_transform_property_count() == 1).then_some(None);
        }
        let resolved = host.resolved_keyframe_value(*property, value)?;
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
    // The compositor interpolates transform lists function by function, so a function that is the same at the end of
    // every list applies after whatever the others have interpolated to.
    let (x, y) = request.additional_translation;
    if x != 0.0 || y != 0.0 {
        let scale = request.device_pixels_per_css_pixel();
        operations.push(VisualAnimationTransformOperation::new(
            FfiVisualAnimationTransformOperationKind::Translate,
            &[x * scale, y * scale],
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
    for keyframe in request.keyframes() {
        let declared @ KeyframeValueOf::Declared(_) = request.value_of(keyframe, property) else {
            continue;
        };
        let Some(candidate) = host.resolved_keyframe_value(property, declared) else {
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
            .keyframes()
            .iter()
            .any(|keyframe| !request.replaces(keyframe) || matches!(keyframe.easing, KeyframeEasing::Value(_)))
        {
            return outcome;
        }

        let Some(values) = self.resolved_values_for_target(request, host).values.as_ref() else {
            return outcome;
        };
        debug_assert_eq!(values.len(), request.keyframes().len());
        let keyframes: Vec<VisualAnimationKeyframe> = request
            .keyframes()
            .iter()
            .zip(values)
            .filter_map(|(keyframe, value)| {
                let value = value.as_ref()?.clone();
                Some(VisualAnimationKeyframe {
                    #[expect(clippy::cast_precision_loss)]
                    offset: offset_of_key(keyframe.key as f64),
                    easing: request.easing_of(keyframe)?,
                    value,
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
            timing: timing.effect,
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::animations::timing::{AnimationEffectTiming, FillMode, TimeValue};
    use crate::css::easing::{FfiEasingDescriptor, FfiEasingKind, FfiLinearEasingPoint};
    use crate::css::style_value::{ColorBase, CssString, RetainedStyleValueDataList};
    use crate::css::style_value::{ColorFunctionPayload, StyleValueBox};
    use crate::css::style_value_arc::StyleValueArc;
    use crate::css::table_group_builder::angle_unit_index;
    use crate::painting::visual_context::publish_compositor_animations;
    use crate::painting::visual_context::{TransformData, TransformDataRole, VisualContextState, VisualContextTree};
    use FfiVisualAnimationTransformOperationKind as Kind;
    use libgfx_rust::{FloatMatrix4x4, FloatPoint};
    use std::cell::RefCell;
    use std::collections::HashMap;
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
        StyleValueData::Transformation(StyleValueBox::new(TransformationPayload {
            property: property_id::TRANSFORM,
            transform_function: function,
            values: RetainedStyleValueDataList::from_retained_values(values.into_iter().map(retained).collect()),
            matrix_values: crate::css::style_value::RetainedStyleValueDataList::from_retained_values(Vec::new()),
        }))
    }

    fn value_list(values: Vec<StyleValueData>) -> StyleValueData {
        StyleValueData::ValueList {
            values: RetainedStyleValueDataList::from_retained_values(values.into_iter().map(retained).collect()),
            separator: 0,
            collapsible: false,
        }
    }

    fn color_function(syntax: u8) -> StyleValueData {
        StyleValueData::ColorFunction(StyleValueBox::new(ColorFunctionPayload {
            color_base: ColorBase {
                has_color_type: true,
                color_type: crate::css::color_conversion::RGB,
                color_syntax: syntax,
            },
            channel_0: retained(number_value(255.0)),
            channel_1: retained(number_value(0.0)),
            channel_2: retained(number_value(0.0)),
            alpha: retained(number_value(1.0)),
            has_name: false,
            name: CssString::default(),
            origin_color: RetainedStyleValueData::none(),
        }))
    }

    fn filter(kind: u8, color_operation: u8, value: StyleValueData) -> StyleValueData {
        StyleValueData::Filter {
            kind,
            color_operation,
            value: retained(value),
        }
    }

    fn shadow(color: Option<StyleValueData>, offset_x: f64, offset_y: f64, blur_radius: Option<f64>) -> StyleValueData {
        StyleValueData::Shadow(StyleValueBox::new(ShadowPayload {
            shadow_type: 0,
            color: color.map_or_else(RetainedStyleValueData::none, retained),
            offset_x: retained(px(offset_x)),
            offset_y: retained(px(offset_y)),
            blur_radius: blur_radius.map_or_else(RetainedStyleValueData::none, |radius| retained(px(radius))),
            spread_distance: RetainedStyleValueData::none(),
            placement: 0,
        }))
    }

    /// A main thread whose keyframe values are given up front, counting what the builder asks for. Each keyframe of a
    /// test declares its index as the value of a property, which the main thread resolves to the value given for it.
    #[derive(Default)]
    struct TestHost {
        values: HashMap<(usize, u16), StyleValueArc>,
        underlying_values: HashMap<u16, StyleValueArc>,
        resolutions: RefCell<usize>,
    }

    impl TestHost {
        fn with_value(mut self, keyframe_index: usize, property: AnimatedProperty, value: StyleValueData) -> Self {
            self.values
                .insert((keyframe_index, property.property_id()), StyleValueArc::new(value));
            self
        }

        fn with_underlying_value(mut self, property: AnimatedProperty, value: StyleValueData) -> Self {
            self.underlying_values
                .insert(property.property_id(), StyleValueArc::new(value));
            self
        }

        fn ffi(&self) -> FfiCompositorAnimationHost {
            FfiCompositorAnimationHost {
                context: std::ptr::from_ref(self).cast_mut().cast(),
                resolved_keyframe_value: test_resolved_keyframe_value,
            }
        }
    }

    unsafe extern "C" fn test_resolved_keyframe_value(
        context: *mut std::ffi::c_void,
        property_id: u16,
        declared_value: *const std::ffi::c_void,
    ) -> *const std::ffi::c_void {
        let host = unsafe { &*context.cast::<TestHost>() };
        *host.resolutions.borrow_mut() += 1;
        let value = match unsafe { declared_value.cast::<StyleValueData>().as_ref() } {
            None => host.underlying_values.get(&property_id),
            Some(&StyleValueData::Integer { value: keyframe_index }) => {
                host.values.get(&(keyframe_index as usize, property_id))
            }
            Some(_) => unreachable!("a test keyframe declares its index"),
        };
        value.map_or(std::ptr::null(), |value| {
            StyleValueArc::into_raw(StyleValueArc::clone(value)).cast()
        })
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

    /// What a test keyframe gives its property.
    #[derive(Clone, Copy)]
    enum ValueState {
        Absent,
        UsesUnderlyingStyle,
        Present,
    }

    /// One keyframe of a test effect, which gives one property a value.
    #[derive(Clone)]
    struct TestKeyframe {
        offset: f64,
        property: AnimatedProperty,
        state: ValueState,
        composite: CompositeOperationOrAuto,
        easing: KeyframeEasing,
    }

    fn keyframe(offset: f64, property: AnimatedProperty, state: ValueState) -> TestKeyframe {
        TestKeyframe {
            offset,
            property,
            state,
            composite: CompositeOperationOrAuto::Auto,
            easing: KeyframeEasing::None,
        }
    }

    /// The keyframe set of `keyframes`, exactly as given, each declaring its index as the value it gives.
    fn keyframe_set(keyframes: &[TestKeyframe]) -> Arc<KeyframeSet> {
        use crate::animations::keyframes::KeyframeProperty;
        Arc::new(KeyframeSet::from_keyframes_as_given(
            keyframes
                .iter()
                .enumerate()
                .map(|(index, keyframe)| {
                    let property = KeyframeProperty::Id(keyframe.property.property_id());
                    let declarations = match keyframe.state {
                        ValueState::Absent => Vec::new(),
                        ValueState::UsesUnderlyingStyle => vec![(property, KeyframeValue::ElementValue)],
                        ValueState::Present => vec![(
                            property,
                            KeyframeValue::Declared(retained(StyleValueData::Integer {
                                value: i32::try_from(index).unwrap(),
                            })),
                        )],
                    };
                    let key = crate::animations::keyframes::key_of_offset(keyframe.offset);
                    (key, keyframe.composite, keyframe.easing.clone(), declarations)
                })
                .collect(),
        ))
    }

    fn timing() -> crate::painting::host::FfiCompositorAnimationTiming {
        crate::painting::host::FfiCompositorAnimationTiming {
            monotonic_time_at_anchor_ns: 1_000,
            local_time_at_anchor_ms: 0.0,
            playback_rate: 1.0,
            effect: AnimationEffectTiming {
                iteration_duration: TimeValue::milliseconds(1000.0),
                iteration_count: f64::INFINITY,
                fill_mode: FillMode::None,
                ..AnimationEffectTiming::default()
            },
            easing: linear_easing(),
        }
    }

    fn build_request(
        kind: FfiVisualAnimationTargetKind,
        targeted_transform_properties: u8,
    ) -> FfiCompositorAnimationRequest {
        FfiCompositorAnimationRequest {
            target_kind: kind,
            layout_node: crate::layout::node_data::NodeSlotId::INVALID,
            target_inherited_ui_values: std::ptr::null(),
            target_inherited_text_values: std::ptr::null(),
            timing: timing(),
            keyframe_set: std::ptr::null(),
            effect_composite_is_replace: true,
            default_easing: linear_easing(),
            target_style_generation: 1,
            style_environment_version: 1,
            reference_box_width: 200.0,
            reference_box_height: 100.0,
            device_pixels_per_css_pixel: 2.0,
            targeted_transform_properties,
            targets_only_transform: targeted_transform_properties == TARGETED_TRANSFORM_PROPERTY_TRANSFORM,
        }
    }

    fn present(offset: f64, property: AnimatedProperty) -> TestKeyframe {
        keyframe(offset, property, ValueState::Present)
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
        let ffi_request = build_request(FfiVisualAnimationTargetKind::Opacity, 0);
        let set = keyframe_set(&keyframes);
        let request = Request::with_keyframe_set(&ffi_request, &set);
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
        let request = Request::with_keyframe_set(&changed_style, &set);
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
        let ffi_request = build_request(FfiVisualAnimationTargetKind::Opacity, 0);
        let request = Request::with_keyframe_set(&ffi_request, &keyframe_set(&keyframes));
        let mut state = CompositorAnimationEffectState::default();

        let outcome = state.build(&request, &Host::new(&ffi_host), |_| Vec::new());
        assert!(!outcome.built);
        assert!(outcome.missing_visual_context_node);
        assert!(!state.has_pending());

        let mut additive = keyframes.clone();
        additive[1].composite = CompositeOperationOrAuto::Add;
        let ffi_request = build_request(FfiVisualAnimationTargetKind::Opacity, 0);
        let request = Request::with_keyframe_set(&ffi_request, &keyframe_set(&additive));
        assert!(!state.build(&request, &Host::new(&ffi_host), |_| vec![3]).built);

        let mut unsupported_easing = keyframes.clone();
        unsupported_easing[0].easing = KeyframeEasing::Value(retained(none()));
        let ffi_request = build_request(FfiVisualAnimationTargetKind::Opacity, 0);
        let request = Request::with_keyframe_set(&ffi_request, &keyframe_set(&unsupported_easing));
        assert!(!state.build(&request, &Host::new(&ffi_host), |_| vec![3]).built);

        // A keyframe without the property is left out; too few remain for an animation.
        let sparse = [
            present(0.0, AnimatedProperty::Opacity),
            keyframe(1.0, AnimatedProperty::Opacity, ValueState::Absent),
        ];
        let ffi_request = build_request(FfiVisualAnimationTargetKind::Opacity, 0);
        let request = Request::with_keyframe_set(&ffi_request, &keyframe_set(&sparse));
        assert!(!state.build(&request, &Host::new(&ffi_host), |_| vec![3]).built);
    }

    #[test]
    fn opacity_keyframes_that_take_the_underlying_style_build_from_it() {
        let host = TestHost::default()
            .with_underlying_value(AnimatedProperty::Opacity, opacity_value(0.25))
            .with_value(1, AnimatedProperty::Opacity, opacity_value(0.5))
            .with_underlying_value(AnimatedProperty::Opacity, opacity_value(0.25));
        let ffi_host = host.ffi();
        let underlying = ValueState::UsesUnderlyingStyle;
        let keyframes = [
            keyframe(0.0, AnimatedProperty::Opacity, underlying),
            present(0.5, AnimatedProperty::Opacity),
            keyframe(1.0, AnimatedProperty::Opacity, underlying),
        ];
        let ffi_request = build_request(FfiVisualAnimationTargetKind::Opacity, 0);
        let request = Request::with_keyframe_set(&ffi_request, &keyframe_set(&keyframes));
        let mut state = CompositorAnimationEffectState::default();

        assert!(state.build(&request, &Host::new(&ffi_host), |_| vec![1]).built);
        let animation = pending_animation(&state, FfiVisualAnimationTargetKind::Opacity);
        assert_eq!(animation.keyframes[0].value, VisualAnimationValue::Opacity(0.25));
        assert_eq!(animation.keyframes[1].value, VisualAnimationValue::Opacity(0.5));
        assert_eq!(animation.keyframes[2].value, VisualAnimationValue::Opacity(0.25));
    }

    #[test]
    fn transform_precision_inputs_do_not_replace_compositor_interpolation_angles() {
        let mut value = transformation(transform_function::ROTATE, vec![deg(1e30 * 360.0)]);
        assert!(
            transform_operations_from_style_value(
                AnimatedProperty::Transform,
                &value_list(vec![value.clone()]),
                (100.0, 100.0),
                1.0,
                None
            )
            .is_some()
        );
        let StyleValueData::Transformation(transformation_payload) = &mut value else {
            unreachable!()
        };
        let TransformationPayload { matrix_values, .. } = &mut **transformation_payload;
        *matrix_values = RetainedStyleValueDataList::from_retained_values(vec![retained(StyleValueData::Angle {
            value: 1e30,
            unit: crate::css::calc::ANGLE_UNIT_NAMES
                .iter()
                .position(|unit| *unit == "turn")
                .unwrap() as u8,
        })]);
        assert!(
            transform_operations_from_style_value(
                AnimatedProperty::Transform,
                &value_list(vec![value]),
                (100.0, 100.0),
                1.0,
                None
            )
            .is_none()
        );
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
            TARGETED_TRANSFORM_PROPERTY_TRANSFORM,
        );
        let request = Request::with_keyframe_set(&ffi_request, &keyframe_set(&keyframes));
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

        // A translation takes its primitive's two values, and a rotation about one axis the
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
                operation(Kind::Translate, &[3.0, 0.0]),
                operation(Kind::Translate, &[200.0, 0.0]),
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
    fn transform_keyframes_naming_functions_of_one_primitive_interpolate_together() {
        let built = |from: StyleValueData, to: StyleValueData| {
            let host = TestHost::default()
                .with_value(0, AnimatedProperty::Transform, value_list(vec![from]))
                .with_value(1, AnimatedProperty::Transform, value_list(vec![to]));
            let ffi_host = host.ffi();
            let keyframes = [
                present(0.0, AnimatedProperty::Transform),
                present(1.0, AnimatedProperty::Transform),
            ];
            let ffi_request = build_request(
                FfiVisualAnimationTargetKind::Transform,
                TARGETED_TRANSFORM_PROPERTY_TRANSFORM,
            );
            let request = Request::with_keyframe_set(&ffi_request, &keyframe_set(&keyframes));
            let mut state = CompositorAnimationEffectState::default();
            assert!(state.build(&request, &Host::new(&ffi_host), |_| vec![1]).built);
            pending_animation(&state, FfiVisualAnimationTargetKind::Transform)
                .keyframes
                .iter()
                .map(|keyframe| match &keyframe.value {
                    VisualAnimationValue::Transform(operations) => operations[0],
                    _ => unreachable!("a transform animation holds transform values"),
                })
                .collect::<Vec<_>>()
        };

        assert_eq!(
            built(
                transformation(transform_function::TRANSLATE, vec![px(1.0), px(0.0)]),
                transformation(transform_function::TRANSLATE_X, vec![px(3.0)]),
            ),
            [
                operation(Kind::Translate, &[2.0, 0.0]),
                operation(Kind::Translate, &[6.0, 0.0])
            ]
        );
        assert_eq!(
            built(
                transformation(transform_function::TRANSLATE_Y, vec![px(1.0)]),
                transformation(transform_function::TRANSLATE_X, vec![px(3.0)]),
            ),
            [
                operation(Kind::Translate, &[0.0, 2.0]),
                operation(Kind::Translate, &[6.0, 0.0])
            ]
        );
        assert_eq!(
            built(
                transformation(transform_function::SCALE_X, vec![number_value(2.0)]),
                transformation(transform_function::SCALE, vec![number_value(3.0)]),
            ),
            [operation(Kind::Scale, &[2.0, 1.0]), operation(Kind::Scale, &[3.0, 3.0])]
        );
        assert_eq!(
            built(
                transformation(transform_function::TRANSLATE_Z, vec![px(1.0)]),
                transformation(transform_function::TRANSLATE3D, vec![px(0.0), px(0.0), px(3.0)]),
            ),
            [
                operation(Kind::Translate3d, &[0.0, 0.0, 2.0]),
                operation(Kind::Translate3d, &[0.0, 0.0, 6.0])
            ]
        );
        // A 2D function and a 3D one interpolate through the 3D primitive, which is the 2D one where no keyframe
        // leaves the plane.
        assert_eq!(
            built(
                transformation(transform_function::TRANSLATE, vec![px(1.0)]),
                transformation(transform_function::TRANSLATE3D, vec![px(2.0), px(4.0), px(0.0)]),
            ),
            [
                operation(Kind::Translate, &[2.0, 0.0]),
                operation(Kind::Translate, &[4.0, 8.0])
            ]
        );
        assert_eq!(
            built(
                transformation(transform_function::TRANSLATE, vec![px(1.0)]),
                transformation(transform_function::TRANSLATE3D, vec![px(0.0), px(0.0), px(3.0)]),
            ),
            [
                operation(Kind::Translate3d, &[2.0, 0.0, 0.0]),
                operation(Kind::Translate3d, &[0.0, 0.0, 6.0])
            ]
        );
        assert_eq!(
            built(
                transformation(
                    transform_function::SCALE3D,
                    vec![number_value(2.0), number_value(3.0), number_value(1.0)]
                ),
                transformation(transform_function::SCALE, vec![number_value(4.0)]),
            ),
            [operation(Kind::Scale, &[2.0, 3.0]), operation(Kind::Scale, &[4.0, 4.0])]
        );
        assert_eq!(
            built(
                transformation(
                    transform_function::SCALE3D,
                    vec![number_value(2.0), number_value(3.0), number_value(5.0)]
                ),
                transformation(transform_function::SCALE, vec![number_value(4.0)]),
            ),
            [
                operation(Kind::Scale3d, &[2.0, 3.0, 5.0]),
                operation(Kind::Scale3d, &[4.0, 4.0, 1.0])
            ]
        );
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
            .with_underlying_value(AnimatedProperty::Scale, none())
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
            TARGETED_TRANSFORM_PROPERTY_TRANSFORM,
        );
        let request = Request::with_keyframe_set(&ffi_request, &keyframe_set(&keyframes));
        let mut state = CompositorAnimationEffectState::default();
        assert!(state.build(&request, &Host::new(&ffi_host), |_| vec![1]).built);
        let animation = pending_animation(&state, FfiVisualAnimationTargetKind::Transform);
        assert_eq!(
            animation.keyframes[0].value,
            VisualAnimationValue::Transform(vec![
                operation(Kind::Scale, &[1.0, 1.0]),
                operation(Kind::Translate, &[0.0, 0.0])
            ])
        );
        assert!(animation.is_valid());

        // The scale property alone: a keyframe that takes the underlying style, which is none, gets
        // the property's own identity when no other keyframe offers a shape... except the second
        // keyframe does, so that shape is taken.
        let keyframes = [
            keyframe(0.0, AnimatedProperty::Scale, ValueState::UsesUnderlyingStyle),
            present(1.0, AnimatedProperty::Scale),
        ];
        let ffi_request = build_request(
            FfiVisualAnimationTargetKind::Transform,
            TARGETED_TRANSFORM_PROPERTY_SCALE,
        );
        let request = Request::with_keyframe_set(&ffi_request, &keyframe_set(&keyframes));
        let mut state = CompositorAnimationEffectState::default();
        assert!(state.build(&request, &Host::new(&ffi_host), |_| vec![1]).built);
        let animation = pending_animation(&state, FfiVisualAnimationTargetKind::Transform);
        assert_eq!(
            animation.keyframes[0].value,
            VisualAnimationValue::Transform(vec![operation(Kind::Scale, &[1.0, 1.0])])
        );
        assert_eq!(
            animation.keyframes[1].value,
            VisualAnimationValue::Transform(vec![operation(Kind::Scale, &[3.0, 3.0])])
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
            TARGETED_TRANSFORM_PROPERTY_TRANSFORM,
        );
        let request = Request::with_keyframe_set(&ffi_request, &keyframe_set(&keyframes));
        assert!(effect_only_translates_horizontally(&request, &Host::new(&ffi_host)));
        assert!(effect_transform_preserves_axes(&request, &Host::new(&ffi_host)));

        let mut state = CompositorAnimationEffectState::default();
        let outcome = state.build(&request, &Host::new(&ffi_host), |_| vec![1]);
        assert!(outcome.built);
        assert!(outcome.only_translates_horizontally);

        let mut with_rotate = ffi_request;
        with_rotate.targeted_transform_properties |= TARGETED_TRANSFORM_PROPERTY_ROTATE;
        with_rotate.targets_only_transform = false;
        let request = Request::with_keyframe_set(&with_rotate, &keyframe_set(&keyframes));
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
        let request = Request::with_keyframe_set(&ffi_request, &keyframe_set(&keyframes));
        assert!(!effect_only_translates_horizontally(
            &request,
            &Host::new(&vertical_host)
        ));
        assert!(effect_transform_preserves_axes(&request, &Host::new(&vertical_host)));
    }

    #[test]
    fn filter_keyframes_lower_to_filter_functions() {
        let host = TestHost::default()
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
        let ffi_request = build_request(FfiVisualAnimationTargetKind::Filter, 0);
        let request = Request::with_keyframe_set(&ffi_request, &keyframe_set(&keyframes));
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
        let modern = TestHost::default()
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
            let host = TestHost::default()
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
            let ffi_request = build_request(FfiVisualAnimationTargetKind::BackgroundColor, 0);
            let request = Request::with_keyframe_set(&ffi_request, &keyframe_set(&keyframes));
            let mut state = CompositorAnimationEffectState::default();
            let built = state.build(&request, &Host::new(&ffi_host), |_| vec![2]).built;
            built.then(|| pending_animation(&state, FfiVisualAnimationTargetKind::BackgroundColor).clone())
        };
        let legacy = build(color_function(COLOR_SYNTAX_LEGACY)).expect("legacy colors are compositor driven");
        assert_eq!(
            legacy.keyframes[1].value,
            VisualAnimationValue::BackgroundColor(Color::from_rgb(255, 0, 0))
        );
        assert!(build(color_function(1)).is_none());
        assert!(
            build(StyleValueData::Keyword {
                keyword: keyword::CURRENTCOLOR
            })
            .is_none()
        );
        // A keyword that names a color, such as a system color, is taken.
        let system_color = build(StyleValueData::Keyword {
            keyword: keyword::CANVAS,
        })
        .expect("system colors are compositor driven");
        assert_eq!(
            system_color.keyframes[1].value,
            VisualAnimationValue::BackgroundColor(Color::from_rgb(255, 255, 255))
        );
    }

    fn tree_with_one_effect() -> Arc<VisualContextTree> {
        Arc::new(VisualContextTree::create(TransformData {
            matrix: FloatMatrix4x4::identity(),
            origin: FloatPoint { x: 0.0, y: 0.0 },
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
            bounds: libgfx_rust::FloatRect::default(),
        }))
    }

    fn built_opacity_animation(anchor_ns: i64) -> VisualAnimation {
        VisualAnimation {
            node_indices: vec![0],
            monotonic_time_at_anchor_ns: anchor_ns,
            timing: AnimationEffectTiming {
                iteration_duration: TimeValue::milliseconds(1000.0),
                ..VisualAnimation::default().timing
            },
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
        retimed.timing.iteration_duration = TimeValue::milliseconds(500.0);
        state.pending[target_kind_index(FfiVisualAnimationTargetKind::Opacity)] = Some(retimed);
        let changed = state.publish_pending(true);
        assert_eq!(changed[0].monotonic_time_at_anchor_ns, 300);

        state.clear_retained();
        assert!(!state.has_retained());

        let mut visual_context = VisualContextState {
            tree: Some(tree_with_one_effect()),
            ..VisualContextState::default()
        };
        assert!(!publish_compositor_animations(&mut visual_context, Vec::new()).published);

        let outcome = publish_compositor_animations(&mut visual_context, first.clone());
        assert!(outcome.published);
        assert!(outcome.parameters_changed);
        assert!(!outcome.timing_anchors_changed);
        assert!(visual_context.tree.as_ref().unwrap().has_visual_animations());

        assert!(!publish_compositor_animations(&mut visual_context, first).published);

        let mut again = again;
        again[0].monotonic_time_at_anchor_ns = 500;
        let outcome = publish_compositor_animations(&mut visual_context, again);
        assert!(outcome.published);
        assert!(!outcome.parameters_changed);
        assert!(outcome.timing_anchors_changed);

        // Discarding the pass's animations publishes none.
        let outcome = publish_compositor_animations(&mut visual_context, Vec::new());
        assert!(outcome.published);
        assert!(outcome.parameters_changed);
        assert!(!visual_context.tree.as_ref().unwrap().has_visual_animations());
    }
}
