/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Every field of a display list command, by name, with its value, as the display list dump lists them. The command
//! structs list their fields once, for both their bytes and this, and a list that leaves out a field does not compile.

use super::builder::read_command;
use super::commands::*;
use libgfx_rust::*;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ResourceKind {
    Font,
    ImageFrame,
    VideoSink,
    VectorImage,
    Canvas,
    CompositorContext,
}

/// The value of one field, as a dump reads it. A span names bytes of the command's payload, which only the reader of
/// the payload can tell the meaning of by the field's name.
#[derive(Clone, Copy)]
pub enum RecordFieldValue<'a> {
    Bool(bool),
    Integer(i64),
    Float(f32),
    Double(f64),
    Color(Color),
    IntPoint(IntPoint),
    FloatPoint(FloatPoint),
    IntSize(IntSize),
    FloatSize(FloatSize),
    IntRect(IntRect),
    FloatRect(FloatRect),
    CornerRadii(CornerRadii),
    AffineTransform(AffineTransform),
    Enum(&'a dyn std::fmt::Debug),
    SpatialNode(SpatialNodeIndex),
    EffectNode(EffectNodeIndex),
    Resource(ResourceKind, u64),
    Span(DisplayListDataSpan),
    GradientColorStops(DisplayListGradientColorStops),
    Fields(&'a dyn RecordFields),
}

pub trait RecordField {
    fn record_field_value(&self) -> RecordFieldValue<'_>;
    /// Whether the value is the one a field of its type holds when it says nothing, which a dump leaves out.
    fn holds_neutral_value(&self) -> bool;
}

pub trait RecordFields {
    /// Hands `visit` each field in declaration order: its name, its value and whether that is the field's neutral value.
    fn visit_record_fields(&self, visit: &mut dyn FnMut(&'static str, RecordFieldValue<'_>, bool));
}

/// Lists the fields of a command struct, or of a struct a command holds, for both its bytes and its dump. A field whose
/// neutral value is not the one of its type names it after `=`.
#[macro_export]
macro_rules! display_list_record_fields {
    ($name:ident { $($field:ident $(= $neutral:expr)?),* $(,)? }) => {
        $crate::ffi_bytes_fields!($name { $($field),* });

        impl $crate::display_list::record_fields::RecordFields for $name {
            fn visit_record_fields(
                &self,
                visit: &mut dyn FnMut(&'static str, $crate::display_list::record_fields::RecordFieldValue<'_>, bool),
            ) {
                let Self { $($field),* } = self;
                $(
                    visit(
                        stringify!($field),
                        $crate::display_list::record_fields::RecordField::record_field_value($field),
                        $crate::display_list_field_holds_neutral_value!($field $(, $neutral)?),
                    );
                )*
            }
        }
    };
}

#[doc(hidden)]
#[macro_export]
macro_rules! display_list_field_holds_neutral_value {
    ($field:ident) => {
        $crate::display_list::record_fields::RecordField::holds_neutral_value($field)
    };
    ($field:ident, $neutral:expr) => {
        *$field == $neutral
    };
}

macro_rules! record_field_via {
    ($($type:ty => |$value:ident| $field_value:expr, neutral: |$neutral_value:ident| $neutral:expr;)+) => {
        $(impl RecordField for $type {
            fn record_field_value(&self) -> RecordFieldValue<'_> {
                let $value = self;
                $field_value
            }
            fn holds_neutral_value(&self) -> bool {
                let $neutral_value = self;
                $neutral
            }
        })+
    };
}

record_field_via! {
    bool => |value| RecordFieldValue::Bool(*value), neutral: |value| !*value;
    u8 => |value| RecordFieldValue::Integer(i64::from(*value)), neutral: |value| *value == 0;
    i32 => |value| RecordFieldValue::Integer(i64::from(*value)), neutral: |value| *value == 0;
    u32 => |value| RecordFieldValue::Integer(i64::from(*value)), neutral: |value| *value == 0;
    i64 => |value| RecordFieldValue::Integer(*value), neutral: |value| *value == 0;
    u64 => |value| RecordFieldValue::Integer(*value as i64), neutral: |value| *value == 0;
    f32 => |value| RecordFieldValue::Float(*value), neutral: |value| *value == 0.0;
    f64 => |value| RecordFieldValue::Double(*value), neutral: |value| *value == 0.0;
    // Transparent is a color that is painted, as the fills that trigger filters or carry animated backgrounds are.
    Color => |value| RecordFieldValue::Color(*value), neutral: |_value| false;
    IntPoint => |value| RecordFieldValue::IntPoint(*value), neutral: |value| *value == IntPoint::default();
    FloatPoint => |value| RecordFieldValue::FloatPoint(*value), neutral: |value| *value == FloatPoint::default();
    IntSize => |value| RecordFieldValue::IntSize(*value), neutral: |value| *value == IntSize::default();
    FloatSize => |value| RecordFieldValue::FloatSize(*value), neutral: |value| *value == FloatSize::default();
    IntRect => |value| RecordFieldValue::IntRect(*value), neutral: |value| *value == IntRect::default();
    FloatRect => |value| RecordFieldValue::FloatRect(*value), neutral: |value| *value == FloatRect::default();
    CornerRadii => |value| RecordFieldValue::CornerRadii(*value), neutral: |value| !value.has_any_radius();
    AffineTransform => |value| RecordFieldValue::AffineTransform(*value), neutral: |value| value.is_identity();
    SpatialNodeIndex => |value| RecordFieldValue::SpatialNode(*value), neutral: |_value| false;
    EffectNodeIndex => |value| RecordFieldValue::EffectNode(*value), neutral: |value| value.is_none();
    FontResourceId => |value| RecordFieldValue::Resource(ResourceKind::Font, value.0), neutral: |_value| false;
    ImageFrameResourceId => |value| RecordFieldValue::Resource(ResourceKind::ImageFrame, value.0), neutral: |_value| false;
    VideoSinkHandle => |value| RecordFieldValue::Resource(ResourceKind::VideoSink, value.0), neutral: |_value| false;
    DisplayListResourceId => |value| RecordFieldValue::Resource(ResourceKind::VectorImage, value.0), neutral: |_value| false;
    CanvasId => |value| RecordFieldValue::Resource(ResourceKind::Canvas, value.0), neutral: |_value| false;
    CompositorContextId => |value| RecordFieldValue::Resource(ResourceKind::CompositorContext, value.0), neutral: |_value| false;
    DisplayListDataSpan => |value| RecordFieldValue::Span(*value), neutral: |value| value.is_empty();
}

macro_rules! record_field_enum {
    ($($type:ty),+ $(,)?) => {
        $(impl RecordField for $type {
            fn record_field_value(&self) -> RecordFieldValue<'_> {
                RecordFieldValue::Enum(self)
            }
            fn holds_neutral_value(&self) -> bool {
                *self == <$type>::default()
            }
        })+
    };
}

record_field_enum!(
    CapStyle,
    ClipMode,
    CompositingAndBlendingOperator,
    InterpolationColorSpace,
    JoinStyle,
    LineStyle,
    MaskKind,
    Orientation,
    GlyphRunOrientation,
    ScalingMode,
    ShouldAntiAlias,
    WindingRule,
    GradientInterpolationType,
    RectangularColorSpace,
    PolarColorSpace,
    HueInterpolationMethod,
    PathPaintKind,
    DisplayListPaintStyleType,
    DisplayListGradientSpreadMethod,
    Canvas2DPaintKind,
    Canvas2DPatternRepetition,
);

// The optional values hold their value or nothing, which is neutral.
macro_rules! record_field_optional {
    ($($type:ty),+ $(,)?) => {
        $(impl RecordField for $type {
            fn record_field_value(&self) -> RecordFieldValue<'_> {
                let Self { value, has_value: _ } = self;
                value.record_field_value()
            }
            fn holds_neutral_value(&self) -> bool {
                !self.has_value
            }
        })+
    };
}

record_field_optional!(
    OptionalFloatRect,
    OptionalColor,
    OptionalU32,
    OptionalF32,
    OptionalAffineTransform
);

impl RecordField for DisplayListGradientColorStops {
    fn record_field_value(&self) -> RecordFieldValue<'_> {
        let Self {
            colors: _,
            positions: _,
            repeating: _,
        } = self;
        RecordFieldValue::GradientColorStops(*self)
    }
    fn holds_neutral_value(&self) -> bool {
        self.colors.is_empty() && self.positions.is_empty() && !self.repeating
    }
}

macro_rules! record_field_with_fields {
    ($($type:ty),+ $(,)?) => {
        $(impl RecordField for $type {
            fn record_field_value(&self) -> RecordFieldValue<'_> {
                RecordFieldValue::Fields(self)
            }
            fn holds_neutral_value(&self) -> bool {
                let mut holds_neutral_values = true;
                self.visit_record_fields(&mut |_, _, holds_neutral_value| holds_neutral_values &= holds_neutral_value);
                holds_neutral_values
            }
        })+
    };
}

record_field_with_fields!(
    GradientInterpolationMethod,
    DisplayListGradientPaintStyle,
    DisplayListPaintStyle,
    Repeat,
    TextShadowLayer,
    Canvas2DPaint,
    Canvas2DShadow,
    Canvas2DCompositing,
);

impl RecordFields for GradientInterpolationMethod {
    fn visit_record_fields(&self, visit: &mut dyn FnMut(&'static str, RecordFieldValue<'_>, bool)) {
        let Self {
            interpolation_type,
            rectangular_color_space,
            polar_color_space,
            hue_interpolation_method,
        } = self;
        visit(
            "interpolation_type",
            interpolation_type.record_field_value(),
            interpolation_type.holds_neutral_value(),
        );
        visit(
            "rectangular_color_space",
            rectangular_color_space.record_field_value(),
            rectangular_color_space.holds_neutral_value(),
        );
        visit(
            "polar_color_space",
            polar_color_space.record_field_value(),
            polar_color_space.holds_neutral_value(),
        );
        visit(
            "hue_interpolation_method",
            hue_interpolation_method.record_field_value(),
            hue_interpolation_method.holds_neutral_value(),
        );
    }
}

/// Hands `visit` the fields of the command of `command_type` whose payload starts at `payload`.
pub fn visit_command_fields(
    command_type: DisplayListCommandType,
    payload: &[u8],
    visit: &mut dyn FnMut(&'static str, RecordFieldValue<'_>, bool),
) {
    fn of<C: DisplayListCommand>(payload: &[u8], visit: &mut dyn FnMut(&'static str, RecordFieldValue<'_>, bool)) {
        read_command::<C>(payload).visit_record_fields(visit);
    }
    use DisplayListCommandType as T;
    match command_type {
        T::DrawGlyphRun => of::<DrawGlyphRun>(payload, visit),
        T::FillRect => of::<FillRect>(payload, visit),
        T::PaintCaret => of::<PaintCaret>(payload, visit),
        T::DrawScaledDecodedImageFrame => of::<DrawScaledDecodedImageFrame>(payload, visit),
        T::DrawRepeatedDecodedImageFrame => of::<DrawRepeatedDecodedImageFrame>(payload, visit),
        T::DrawRepeatedTile => of::<DrawRepeatedTile>(payload, visit),
        T::DrawTiledDecodedImageFrame => of::<DrawTiledDecodedImageFrame>(payload, visit),
        T::DrawCompositedContext => of::<DrawCompositedContext>(payload, visit),
        T::DrawCanvas => of::<DrawCanvas>(payload, visit),
        T::DrawVideoFrame => of::<DrawVideoFrame>(payload, visit),
        T::PaintLinearGradient => of::<PaintLinearGradient>(payload, visit),
        T::PaintRadialGradient => of::<PaintRadialGradient>(payload, visit),
        T::PaintConicGradient => of::<PaintConicGradient>(payload, visit),
        T::PaintOuterBoxShadow => of::<PaintOuterBoxShadow>(payload, visit),
        T::PaintInnerBoxShadow => of::<PaintInnerBoxShadow>(payload, visit),
        T::PaintPathBoxShadow => of::<PaintPathBoxShadow>(payload, visit),
        T::PaintTextShadow => of::<PaintTextShadow>(payload, visit),
        T::FillRectWithRoundedCorners => of::<FillRectWithRoundedCorners>(payload, visit),
        T::FillRoundedRectRing => of::<FillRoundedRectRing>(payload, visit),
        T::FillPath => of::<FillPath>(payload, visit),
        T::StrokePath => of::<StrokePath>(payload, visit),
        T::DrawEllipse => of::<DrawEllipse>(payload, visit),
        T::DrawLine => of::<DrawLine>(payload, visit),
        T::BackdropFilterRegion => of::<BackdropFilterRegion>(payload, visit),
        T::DrawRect => of::<DrawRect>(payload, visit),
        T::PaintVectorImage => of::<PaintVectorImage>(payload, visit),
        T::DrawIsolatedGroup => of::<DrawIsolatedGroup>(payload, visit),
        T::DeclareMaskContent => of::<DeclareMaskContent>(payload, visit),
        T::CompositorWheelHitTestTarget => of::<CompositorWheelHitTestTarget>(payload, visit),
        T::CompositorWheelHitTestTargetWithCornerRadii => {
            of::<CompositorWheelHitTestTargetWithCornerRadii>(payload, visit);
        }
        T::CompositorMainThreadWheelEventRegion => of::<CompositorMainThreadWheelEventRegion>(payload, visit),
        T::CompositorScrollbar => of::<CompositorScrollbar>(payload, visit),
        T::CompositorBlockingWheelEventRegion => of::<CompositorBlockingWheelEventRegion>(payload, visit),
        T::PaintScrollBar => of::<PaintScrollBar>(payload, visit),
        T::CompositorNestedContextWheelRegion => of::<CompositorNestedContextWheelRegion>(payload, visit),
        T::Canvas2DFillPath => of::<Canvas2DFillPath>(payload, visit),
        T::Canvas2DStrokePath => of::<Canvas2DStrokePath>(payload, visit),
        T::Canvas2DDrawGlyphRuns => of::<Canvas2DDrawGlyphRuns>(payload, visit),
        T::Canvas2DClearRect => of::<Canvas2DClearRect>(payload, visit),
        T::Canvas2DDrawImageFrame => of::<Canvas2DDrawImageFrame>(payload, visit),
        T::Canvas2DDrawCanvas => of::<Canvas2DDrawCanvas>(payload, visit),
        T::Canvas2DDrawVideoFrame => of::<Canvas2DDrawVideoFrame>(payload, visit),
        T::Canvas2DSetTransform => of::<Canvas2DSetTransform>(payload, visit),
        T::Canvas2DSave => of::<Canvas2DSave>(payload, visit),
        T::Canvas2DRestore => of::<Canvas2DRestore>(payload, visit),
        T::Canvas2DClipPath => of::<Canvas2DClipPath>(payload, visit),
        T::Canvas2DReset => of::<Canvas2DReset>(payload, visit),
        T::Canvas2DClearCanvas => of::<Canvas2DClearCanvas>(payload, visit),
        T::Canvas2DPutImageFrame => of::<Canvas2DPutImageFrame>(payload, visit),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn visited_fields(fields: &dyn RecordFields) -> Vec<(&'static str, bool)> {
        let mut visited = Vec::new();
        fields.visit_record_fields(&mut |name, _, holds_neutral_value| visited.push((name, holds_neutral_value)));
        visited
    }

    #[test]
    fn a_command_lists_every_field_in_declaration_order_with_its_neutral_value() {
        let fill = FillRect {
            rect: IntRect::new(1, 2, 3, 4),
            color: Color::TRANSPARENT,
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            background_color_animation_effect: EffectNodeIndex::NONE,
        };
        assert_eq!(
            visited_fields(&fill),
            [
                ("rect", false),
                ("color", false),
                ("compositing_and_blending_operator", true),
                ("background_color_animation_effect", true),
            ]
        );
    }

    #[test]
    fn a_field_with_a_neutral_value_of_its_own_is_neutral_only_at_that_value() {
        let mut group = DrawIsolatedGroup {
            clip_rect: FloatRect::default(),
            content: DisplayListDataSpan::default(),
            mask: DisplayListDataSpan::default(),
            filter: DisplayListDataSpan::default(),
            opacity: 1.0,
            compositing_and_blending_operator: CompositingAndBlendingOperator::Multiply,
            mask_kind: MaskKind::Alpha,
        };
        let opacity_is_neutral = |group: &DrawIsolatedGroup| {
            visited_fields(group)
                .into_iter()
                .find(|(name, _)| *name == "opacity")
                .unwrap()
                .1
        };
        assert!(opacity_is_neutral(&group));
        group.opacity = 0.0;
        assert!(!opacity_is_neutral(&group));
    }
}
