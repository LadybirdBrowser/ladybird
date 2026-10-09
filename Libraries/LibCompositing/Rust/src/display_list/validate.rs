/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Checks a display list tape that comes from another process before anything reads it. A tape that passes has
//! well formed records, valid enum and bool bytes, and spans inside their payloads, so the typed reads that replay,
//! damage and the player make of it are sound.

use super::builder::{COMMAND_ALIGNMENT, HEADER_SIZE, read_header};
use super::commands::*;
use libgfx_rust::{
    CapStyle, Color, CompositingAndBlendingOperator, GradientInterpolationMethod, GradientInterpolationType,
    HueInterpolationMethod, IntRect, InterpolationColorSpace, JoinStyle, LineStyle, MaskKind, Orientation,
    PolarColorSpace, RectangularColorSpace, ScalingMode, ShouldAntiAlias, WindingRule,
};
use std::mem::{align_of, offset_of, size_of};

pub type ValidationError = &'static str;

const RUN_SIZE: usize = size_of::<DisplayListCommandRun>();

// An enum that a tape stores as its raw discriminant.
trait TapeEnum {
    const SIZE: usize;
    fn is_valid(raw: i64) -> bool;
}

macro_rules! tape_enum {
    ($name:ident as $repr:ty { $($variant:ident),+ $(,)? }) => {
        impl TapeEnum for $name {
            const SIZE: usize = size_of::<$repr>();
            fn is_valid(raw: i64) -> bool {
                // A variant added to the enum does not compile here until it is listed.
                fn _every_variant_is_listed(value: $name) {
                    match value {
                        $($name::$variant => {})+
                    }
                }
                $(raw == $name::$variant as $repr as i64)||+
            }
        }
    };
}

tape_enum!(InlineClipKind as u8 { Rect, RoundedRect, Path });
tape_enum!(ClipMode as u8 { Intersect, Difference });
tape_enum!(CompositorScrollNodeKind as u8 { Viewport, Element, PseudoElement });
tape_enum!(PathPaintKind as u8 { Color, PaintStyle });
tape_enum!(DisplayListPaintStyleType as u8 { None, LinearGradient, RadialGradient, Pattern });
tape_enum!(DisplayListGradientSpreadMethod as u8 { Pad, Repeat, Reflect });
tape_enum!(WindingRule as i32 { Nonzero, EvenOdd });
tape_enum!(LineStyle as u8 { Solid, Dotted, Dashed });
tape_enum!(ScalingMode as i32 { None, Bilinear, BilinearMipmap, NearestNeighbor });
tape_enum!(MaskKind as i32 { Alpha, Luminance });
tape_enum!(ShouldAntiAlias as u8 { Yes, No });
tape_enum!(InterpolationColorSpace as i32 { LinearRGB, SRGB });
tape_enum!(CapStyle as i32 { Butt, Round, Square });
tape_enum!(JoinStyle as i32 { Miter, Round, Bevel });
tape_enum!(Orientation as i32 { Horizontal, Vertical });
tape_enum!(GradientInterpolationType as u8 { Rectangular, Polar });
tape_enum!(RectangularColorSpace as u8 {
    Srgb,
    SrgbLinear,
    DisplayP3,
    DisplayP3Linear,
    A98Rgb,
    ProphotoRgb,
    Rec2020,
    Lab,
    Oklab,
    Xyz,
    XyzD50,
    XyzD65,
});
tape_enum!(PolarColorSpace as u8 { Hsl, Hwb, Lch, Oklch });
tape_enum!(HueInterpolationMethod as u8 { Shorter, Longer, Increasing, Decreasing });

impl TapeEnum for CompositingAndBlendingOperator {
    const SIZE: usize = size_of::<i32>();
    fn is_valid(raw: i64) -> bool {
        i32::try_from(raw).ok().and_then(Self::from_i32).is_some()
    }
}

// The values of SnapStrictness and SnapAlign in LibCompositing/Scrolling/ScrollSnapSelection.h.
const SNAP_STRICTNESS_COUNT: u8 = 3;
const SNAP_ALIGN_COUNT: u8 = 4;

// The bytes of one command payload. Offsets are relative to the payload start, where the command struct begins.
#[derive(Clone, Copy)]
struct Payload<'a> {
    bytes: &'a [u8],
}

impl Payload<'_> {
    fn u8_at(self, offset: usize) -> u8 {
        self.bytes[offset]
    }

    fn u32_at(self, offset: usize) -> u32 {
        u32::from_ne_bytes(self.bytes[offset..offset + 4].try_into().unwrap())
    }

    fn i32_at(self, offset: usize) -> i32 {
        i32::from_ne_bytes(self.bytes[offset..offset + 4].try_into().unwrap())
    }

    fn bool_at(self, offset: usize) -> Result<(), ValidationError> {
        if self.u8_at(offset) > 1 {
            return Err("Display list command has an invalid bool");
        }
        Ok(())
    }

    fn enum_at<E: TapeEnum>(self, offset: usize) -> Result<(), ValidationError> {
        let raw = match E::SIZE {
            1 => i64::from(self.u8_at(offset)),
            4 => i64::from(self.i32_at(offset)),
            _ => unreachable!(),
        };
        if !E::is_valid(raw) {
            return Err("Display list command has an invalid enum value");
        }
        Ok(())
    }

    fn u8_below(self, offset: usize, count: u8) -> Result<(), ValidationError> {
        if self.u8_at(offset) >= count {
            return Err("Display list command has an invalid enum value");
        }
        Ok(())
    }

    // A span of opaque bytes, which must lie inside the payload.
    fn span_at(self, offset: usize) -> Result<(usize, usize), ValidationError> {
        let start = self.u32_at(offset + offset_of!(DisplayListDataSpan, offset)) as usize;
        let size = self.u32_at(offset + offset_of!(DisplayListDataSpan, size)) as usize;
        if start.checked_add(size).is_none_or(|end| end > self.bytes.len()) {
            return Err("Display list span exceeds its command payload");
        }
        Ok((start, size))
    }

    // A span of `T` values, which the player reads in place.
    fn array_span_at<T>(self, offset: usize) -> Result<usize, ValidationError> {
        let (start, size) = self.span_at(offset)?;
        if !size.is_multiple_of(size_of::<T>()) || !start.is_multiple_of(align_of::<T>()) {
            return Err("Display list array span is not a whole number of aligned values");
        }
        Ok(size / size_of::<T>())
    }
}

// Collects the nested record spans of one payload, which must not overlap each other, the command struct or the
// tail entries. Each nested byte then belongs to one record stream, and checking every stream stays linear.
struct NestedSpans<'a> {
    payload: Payload<'a>,
    struct_size: usize,
    entries_offset: usize,
    spans: Vec<(usize, usize)>,
}

impl NestedSpans<'_> {
    fn add(&mut self, offset: usize) -> Result<(), ValidationError> {
        let (start, size) = self.payload.span_at(offset)?;
        if size == 0 {
            return Ok(());
        }
        if !start.is_multiple_of(COMMAND_ALIGNMENT) || !size.is_multiple_of(COMMAND_ALIGNMENT) {
            return Err("Nested display list records are not aligned");
        }
        if start < self.struct_size || start + size > self.entries_offset {
            return Err("Nested display list records overlap their command");
        }
        if self
            .spans
            .iter()
            .any(|&(other_start, other_size)| start < other_start + other_size && other_start < start + size)
        {
            return Err("Nested display list records overlap each other");
        }
        self.spans.push((start, size));
        Ok(())
    }
}

fn validate_color_stops(payload: Payload, offset: usize, must_have_stops: bool) -> Result<(), ValidationError> {
    let colors = payload.array_span_at::<Color>(offset + offset_of!(DisplayListGradientColorStops, colors))?;
    let positions = payload.array_span_at::<f32>(offset + offset_of!(DisplayListGradientColorStops, positions))?;
    payload.bool_at(offset + offset_of!(DisplayListGradientColorStops, repeating))?;
    if colors != positions {
        return Err("Display list gradient has a different number of colors and positions");
    }
    if must_have_stops && colors == 0 {
        return Err("Display list gradient has no color stops");
    }
    Ok(())
}

fn validate_interpolation_method(payload: Payload, offset: usize) -> Result<(), ValidationError> {
    payload
        .enum_at::<GradientInterpolationType>(offset + offset_of!(GradientInterpolationMethod, interpolation_type))?;
    payload
        .enum_at::<RectangularColorSpace>(offset + offset_of!(GradientInterpolationMethod, rectangular_color_space))?;
    payload.enum_at::<PolarColorSpace>(offset + offset_of!(GradientInterpolationMethod, polar_color_space))?;
    payload
        .enum_at::<HueInterpolationMethod>(offset + offset_of!(GradientInterpolationMethod, hue_interpolation_method))
}

fn validate_optional_at(payload: Payload, offset: usize, has_value_offset: usize) -> Result<(), ValidationError> {
    payload.bool_at(offset + has_value_offset)
}

fn validate_paint_style(
    payload: Payload,
    offset: usize,
    paint_kind_offset: usize,
    nested: &mut NestedSpans,
) -> Result<(), ValidationError> {
    payload.enum_at::<PathPaintKind>(paint_kind_offset)?;
    let style_type_offset = offset + offset_of!(DisplayListPaintStyle, paint_style_type);
    payload.enum_at::<DisplayListPaintStyleType>(style_type_offset)?;
    let gradient = offset + offset_of!(DisplayListPaintStyle, gradient);
    validate_optional_at(
        payload,
        gradient + offset_of!(DisplayListGradientPaintStyle, gradient_transform),
        offset_of!(OptionalAffineTransform, has_value),
    )?;
    payload.enum_at::<DisplayListGradientSpreadMethod>(
        gradient + offset_of!(DisplayListGradientPaintStyle, spread_method),
    )?;
    payload.enum_at::<InterpolationColorSpace>(gradient + offset_of!(DisplayListGradientPaintStyle, color_space))?;
    validate_color_stops(
        payload,
        gradient + offset_of!(DisplayListGradientPaintStyle, color_stops),
        false,
    )?;
    validate_optional_at(
        payload,
        offset + offset_of!(DisplayListPaintStyle, pattern_transform),
        offset_of!(OptionalAffineTransform, has_value),
    )?;
    let pattern_tile = offset + offset_of!(DisplayListPaintStyle, pattern_tile);
    let paints_pattern = payload.u8_at(paint_kind_offset) == PathPaintKind::PaintStyle as u8
        && payload.u8_at(style_type_offset) == DisplayListPaintStyleType::Pattern as u8;
    if paints_pattern {
        nested.add(pattern_tile)
    } else {
        payload.span_at(pattern_tile).map(|_| ())
    }
}

fn command_struct_size(command_type: DisplayListCommandType) -> usize {
    use DisplayListCommandType as T;
    match command_type {
        T::DrawGlyphRun => size_of::<DrawGlyphRun>(),
        T::FillRect => size_of::<FillRect>(),
        T::PaintCaret => size_of::<PaintCaret>(),
        T::DrawScaledDecodedImageFrame => size_of::<DrawScaledDecodedImageFrame>(),
        T::DrawRepeatedDecodedImageFrame => size_of::<DrawRepeatedDecodedImageFrame>(),
        T::DrawRepeatedTile => size_of::<DrawRepeatedTile>(),
        T::DrawTiledDecodedImageFrame => size_of::<DrawTiledDecodedImageFrame>(),
        T::DrawCompositedContext => size_of::<DrawCompositedContext>(),
        T::DrawCanvas => size_of::<DrawCanvas>(),
        T::DrawVideoFrame => size_of::<DrawVideoFrame>(),
        T::PaintLinearGradient => size_of::<PaintLinearGradient>(),
        T::PaintRadialGradient => size_of::<PaintRadialGradient>(),
        T::PaintConicGradient => size_of::<PaintConicGradient>(),
        T::PaintOuterBoxShadow => size_of::<PaintOuterBoxShadow>(),
        T::PaintInnerBoxShadow => size_of::<PaintInnerBoxShadow>(),
        T::PaintTextShadow => size_of::<PaintTextShadow>(),
        T::FillRectWithRoundedCorners => size_of::<FillRectWithRoundedCorners>(),
        T::FillRoundedRectRing => size_of::<FillRoundedRectRing>(),
        T::FillPath => size_of::<FillPath>(),
        T::StrokePath => size_of::<StrokePath>(),
        T::DrawEllipse => size_of::<DrawEllipse>(),
        T::DrawLine => size_of::<DrawLine>(),
        T::BackdropFilterRegion => size_of::<BackdropFilterRegion>(),
        T::DrawRect => size_of::<DrawRect>(),
        T::PaintNestedDisplayList => size_of::<PaintNestedDisplayList>(),
        T::DrawIsolatedGroup => size_of::<DrawIsolatedGroup>(),
        T::DeclareMaskContent => size_of::<DeclareMaskContent>(),
        T::CompositorScrollNode => size_of::<CompositorScrollNode>(),
        T::CompositorWheelHitTestTarget => size_of::<CompositorWheelHitTestTarget>(),
        T::CompositorWheelHitTestTargetWithCornerRadii => size_of::<CompositorWheelHitTestTargetWithCornerRadii>(),
        T::CompositorMainThreadWheelEventRegion => size_of::<CompositorMainThreadWheelEventRegion>(),
        T::CompositorScrollbar => size_of::<CompositorScrollbar>(),
        T::CompositorBlockingWheelEventRegion => size_of::<CompositorBlockingWheelEventRegion>(),
        T::PaintScrollBar => size_of::<PaintScrollBar>(),
        T::CompositorSnapContainer => size_of::<CompositorSnapContainer>(),
        T::CompositorSnapArea => size_of::<CompositorSnapArea>(),
    }
}

// Checks the fields of the command struct at the start of `payload` whose bytes could hold an invalid value.
fn validate_command_fields(
    command_type: DisplayListCommandType,
    payload: Payload,
    nested: &mut NestedSpans,
) -> Result<(), ValidationError> {
    use DisplayListCommandType as T;
    let p = payload;
    match command_type {
        T::DrawGlyphRun => {
            p.array_span_at::<DisplayListGlyph>(offset_of!(DrawGlyphRun, glyphs))?;
            p.enum_at::<Orientation>(offset_of!(DrawGlyphRun, orientation))?;
        }
        T::FillRect => {
            p.enum_at::<CompositingAndBlendingOperator>(offset_of!(FillRect, compositing_and_blending_operator))?;
        }
        T::PaintCaret => p.bool_at(offset_of!(PaintCaret, should_blink))?,
        T::DrawScaledDecodedImageFrame => {
            validate_optional_at(
                p,
                offset_of!(DrawScaledDecodedImageFrame, src_rect),
                offset_of!(OptionalFloatRect, has_value),
            )?;
            p.enum_at::<ScalingMode>(offset_of!(DrawScaledDecodedImageFrame, scaling_mode))?;
            p.enum_at::<CompositingAndBlendingOperator>(offset_of!(
                DrawScaledDecodedImageFrame,
                compositing_and_blending_operator
            ))?;
            validate_optional_at(
                p,
                offset_of!(DrawScaledDecodedImageFrame, isolated_backdrop_color),
                offset_of!(OptionalColor, has_value),
            )?;
            p.bool_at(offset_of!(DrawScaledDecodedImageFrame, apply_force_dark))?;
        }
        T::DrawRepeatedDecodedImageFrame => {
            p.enum_at::<ScalingMode>(offset_of!(DrawRepeatedDecodedImageFrame, scaling_mode))?;
            let repeat = offset_of!(DrawRepeatedDecodedImageFrame, repeat);
            p.bool_at(repeat + offset_of!(Repeat, x))?;
            p.bool_at(repeat + offset_of!(Repeat, y))?;
            p.enum_at::<CompositingAndBlendingOperator>(offset_of!(
                DrawRepeatedDecodedImageFrame,
                compositing_and_blending_operator
            ))?;
            validate_optional_at(
                p,
                offset_of!(DrawRepeatedDecodedImageFrame, isolated_backdrop_color),
                offset_of!(OptionalColor, has_value),
            )?;
            p.bool_at(offset_of!(DrawRepeatedDecodedImageFrame, apply_force_dark))?;
        }
        T::DrawRepeatedTile => {
            nested.add(offset_of!(DrawRepeatedTile, tile))?;
            p.enum_at::<ScalingMode>(offset_of!(DrawRepeatedTile, scaling_mode))?;
            p.enum_at::<CompositingAndBlendingOperator>(offset_of!(
                DrawRepeatedTile,
                compositing_and_blending_operator
            ))?;
            let repeat = offset_of!(DrawRepeatedTile, repeat);
            p.bool_at(repeat + offset_of!(Repeat, x))?;
            p.bool_at(repeat + offset_of!(Repeat, y))?;
        }
        T::DrawTiledDecodedImageFrame => {
            p.enum_at::<ScalingMode>(offset_of!(DrawTiledDecodedImageFrame, scaling_mode))?;
            validate_optional_at(
                p,
                offset_of!(DrawTiledDecodedImageFrame, tile_count_x),
                offset_of!(OptionalU32, has_value),
            )?;
            validate_optional_at(
                p,
                offset_of!(DrawTiledDecodedImageFrame, tile_count_y),
                offset_of!(OptionalU32, has_value),
            )?;
            p.bool_at(offset_of!(DrawTiledDecodedImageFrame, apply_force_dark))?;
        }
        T::DrawCompositedContext => p.enum_at::<ScalingMode>(offset_of!(DrawCompositedContext, scaling_mode))?,
        T::DrawCanvas => p.enum_at::<ScalingMode>(offset_of!(DrawCanvas, scaling_mode))?,
        T::DrawVideoFrame => p.enum_at::<ScalingMode>(offset_of!(DrawVideoFrame, scaling_mode))?,
        T::PaintLinearGradient => {
            validate_color_stops(p, offset_of!(PaintLinearGradient, color_stops), true)?;
            validate_interpolation_method(p, offset_of!(PaintLinearGradient, interpolation_method))?;
            p.enum_at::<CompositingAndBlendingOperator>(offset_of!(
                PaintLinearGradient,
                compositing_and_blending_operator
            ))?;
        }
        T::PaintRadialGradient => {
            validate_color_stops(p, offset_of!(PaintRadialGradient, color_stops), true)?;
            validate_interpolation_method(p, offset_of!(PaintRadialGradient, interpolation_method))?;
            p.enum_at::<CompositingAndBlendingOperator>(offset_of!(
                PaintRadialGradient,
                compositing_and_blending_operator
            ))?;
        }
        T::PaintConicGradient => {
            validate_color_stops(p, offset_of!(PaintConicGradient, color_stops), true)?;
            validate_interpolation_method(p, offset_of!(PaintConicGradient, interpolation_method))?;
            p.enum_at::<CompositingAndBlendingOperator>(offset_of!(
                PaintConicGradient,
                compositing_and_blending_operator
            ))?;
        }
        T::PaintTextShadow => {
            p.array_span_at::<DisplayListGlyph>(offset_of!(PaintTextShadow, glyphs))?;
            p.array_span_at::<TextShadowLayer>(offset_of!(PaintTextShadow, layers))?;
            p.enum_at::<Orientation>(offset_of!(PaintTextShadow, orientation))?;
        }
        T::FillPath => {
            p.span_at(offset_of!(FillPath, path_data))?;
            validate_paint_style(
                p,
                offset_of!(FillPath, paint_style),
                offset_of!(FillPath, paint_kind),
                nested,
            )?;
            p.enum_at::<WindingRule>(offset_of!(FillPath, winding_rule))?;
            p.enum_at::<ShouldAntiAlias>(offset_of!(FillPath, should_anti_alias))?;
            p.enum_at::<CompositingAndBlendingOperator>(offset_of!(FillPath, compositing_and_blending_operator))?;
        }
        T::StrokePath => {
            p.enum_at::<CapStyle>(offset_of!(StrokePath, cap_style))?;
            p.enum_at::<JoinStyle>(offset_of!(StrokePath, join_style))?;
            p.array_span_at::<f32>(offset_of!(StrokePath, dash_array))?;
            p.span_at(offset_of!(StrokePath, path_data))?;
            validate_paint_style(
                p,
                offset_of!(StrokePath, paint_style),
                offset_of!(StrokePath, paint_kind),
                nested,
            )?;
            p.enum_at::<ShouldAntiAlias>(offset_of!(StrokePath, should_anti_alias))?;
        }
        T::DrawLine => p.enum_at::<LineStyle>(offset_of!(DrawLine, style))?,
        T::DrawRect => p.bool_at(offset_of!(DrawRect, rough))?,
        T::DrawIsolatedGroup => {
            nested.add(offset_of!(DrawIsolatedGroup, content))?;
            nested.add(offset_of!(DrawIsolatedGroup, mask))?;
            p.span_at(offset_of!(DrawIsolatedGroup, filter))?;
            p.enum_at::<CompositingAndBlendingOperator>(offset_of!(
                DrawIsolatedGroup,
                compositing_and_blending_operator
            ))?;
            p.enum_at::<MaskKind>(offset_of!(DrawIsolatedGroup, mask_kind))?;
        }
        T::DeclareMaskContent => nested.add(offset_of!(DeclareMaskContent, content))?,
        T::CompositorScrollNode => {
            p.enum_at::<CompositorScrollNodeKind>(offset_of!(CompositorScrollNode, scroll_node_kind))?;
            p.bool_at(offset_of!(CompositorScrollNode, is_viewport))?;
            p.bool_at(offset_of!(CompositorScrollNode, can_be_wheel_scrolled_horizontally))?;
            p.bool_at(offset_of!(CompositorScrollNode, can_be_wheel_scrolled_vertically))?;
        }
        T::CompositorScrollbar => {
            p.bool_at(offset_of!(CompositorScrollbar, vertical))?;
            p.bool_at(offset_of!(CompositorScrollbar, is_painted_by_compositor))?;
            p.bool_at(offset_of!(CompositorScrollbar, display_list_paints_enlarged_scrollbar))?;
        }
        T::PaintScrollBar => p.bool_at(offset_of!(PaintScrollBar, vertical))?,
        T::CompositorSnapContainer => {
            p.u8_below(offset_of!(CompositorSnapContainer, strictness), SNAP_STRICTNESS_COUNT)?;
            p.bool_at(offset_of!(CompositorSnapContainer, snaps_x))?;
            p.bool_at(offset_of!(CompositorSnapContainer, snaps_y))?;
            p.bool_at(offset_of!(CompositorSnapContainer, horizontal_writing_mode))?;
        }
        T::CompositorSnapArea => {
            p.u8_below(offset_of!(CompositorSnapArea, align_x), SNAP_ALIGN_COUNT)?;
            p.u8_below(offset_of!(CompositorSnapArea, align_y), SNAP_ALIGN_COUNT)?;
            p.bool_at(offset_of!(CompositorSnapArea, always_stop))?;
        }
        T::PaintOuterBoxShadow
        | T::PaintInnerBoxShadow
        | T::FillRectWithRoundedCorners
        | T::FillRoundedRectRing
        | T::DrawEllipse
        | T::BackdropFilterRegion
        | T::PaintNestedDisplayList
        | T::CompositorWheelHitTestTarget
        | T::CompositorWheelHitTestTargetWithCornerRadii
        | T::CompositorMainThreadWheelEventRegion
        | T::CompositorBlockingWheelEventRegion => {}
    }
    Ok(())
}

fn validate_inline_clip(payload: Payload, offset: usize) -> Result<(), ValidationError> {
    payload.enum_at::<InlineClipKind>(offset + offset_of!(DisplayListInlineClip, kind))?;
    payload.enum_at::<ClipMode>(offset + offset_of!(DisplayListInlineClip, mode))?;
    payload.enum_at::<WindingRule>(offset + offset_of!(DisplayListInlineClip, path_winding_rule))?;
    payload.span_at(offset + offset_of!(DisplayListInlineClip, path_data))?;
    Ok(())
}

// What a run says about its records, recomputed by the rules the builder summarizes them with.
#[derive(Default)]
struct RunSummary {
    ink_bounds: IntRect,
    has_unbounded_draw: bool,
    has_compositor_metadata: bool,
}

impl RunSummary {
    fn note(&mut self, header: &DisplayListCommandHeader) {
        if header.command_type.is_compositor_metadata() {
            self.has_compositor_metadata = true;
        } else if header.has_bounding_rect {
            self.ink_bounds = self.ink_bounds.united(header.bounding_rect);
        } else {
            self.has_unbounded_draw = true;
        }
    }

    fn matches(&self, run: &DisplayListCommandRun) -> bool {
        // Merging runs can leave a different empty rect than one walk over the records does; any empty rect
        // means the same thing.
        let ink_bounds_match =
            self.ink_bounds == run.ink_bounds || (self.ink_bounds.is_empty() && run.ink_bounds.is_empty());
        ink_bounds_match
            && self.has_unbounded_draw == run.has_unbounded_draw
            && self.has_compositor_metadata == run.has_compositor_metadata
    }
}

fn validate_header_bytes(bytes: &[u8]) -> Result<DisplayListCommandHeader, ValidationError> {
    if bytes.len() < HEADER_SIZE {
        return Err("Display list ends inside a command header");
    }
    if DisplayListCommandType::from_u8(bytes[offset_of!(DisplayListCommandHeader, command_type)]).is_none() {
        return Err("Display list command has an invalid type");
    }
    if bytes[offset_of!(DisplayListCommandHeader, has_bounding_rect)] > 1
        || bytes[offset_of!(DisplayListCommandHeader, has_inline_transform)] > 1
    {
        return Err("Display list command header has an invalid bool");
    }
    Ok(read_header(bytes))
}

// Checks one stream of records, and pushes the nested record streams its commands hold onto `pending`.
fn validate_records<'a>(
    records: &'a [u8],
    pending: &mut Vec<&'a [u8]>,
    mut summary: Option<&mut RunSummary>,
) -> Result<(), ValidationError> {
    let mut offset = 0;
    while offset < records.len() {
        let header = validate_header_bytes(&records[offset..])?;
        let payload_size = header.payload_size as usize;
        let payload_start = offset + HEADER_SIZE;
        if !payload_size.is_multiple_of(COMMAND_ALIGNMENT) || payload_size > records.len() - payload_start {
            return Err("Display list command record does not fit its stream");
        }
        let payload = Payload {
            bytes: &records[payload_start..payload_start + payload_size],
        };
        let entries_size = usize::from(header.inline_clip_count) * INLINE_CLIP_ENTRY_SIZE
            + if header.has_inline_transform {
                INLINE_TRANSFORM_ENTRY_SIZE
            } else {
                0
            };
        let struct_size = command_struct_size(header.command_type);
        if entries_size > payload_size || struct_size > payload_size - entries_size {
            return Err("Display list command payload is too small for its command");
        }
        let entries_offset = payload_size - entries_size;
        if !entries_offset.is_multiple_of(COMMAND_ALIGNMENT) {
            return Err("Display list command tail entries are not aligned");
        }
        let mut nested = NestedSpans {
            payload,
            struct_size,
            entries_offset,
            spans: Vec::new(),
        };
        validate_command_fields(header.command_type, payload, &mut nested)?;
        let clips_offset = entries_offset
            + if header.has_inline_transform {
                INLINE_TRANSFORM_ENTRY_SIZE
            } else {
                0
            };
        for index in 0..usize::from(header.inline_clip_count) {
            validate_inline_clip(payload, clips_offset + index * INLINE_CLIP_ENTRY_SIZE)?;
        }
        for (start, size) in nested.spans {
            pending.push(&payload.bytes[start..start + size]);
        }
        if let Some(summary) = summary.as_deref_mut() {
            summary.note(&header);
        }
        offset = payload_start + payload_size;
    }
    Ok(())
}

fn read_run(bytes: &[u8]) -> Result<DisplayListCommandRun, ValidationError> {
    let u32_at = |offset: usize| u32::from_ne_bytes(bytes[offset..offset + 4].try_into().unwrap());
    let i32_at = |offset: usize| i32::from_ne_bytes(bytes[offset..offset + 4].try_into().unwrap());
    let bool_at = |offset: usize| match bytes[offset] {
        0 => Ok(false),
        1 => Ok(true),
        _ => Err("Display list command run has an invalid bool"),
    };
    let context = offset_of!(DisplayListCommandRun, context);
    let ink_bounds = offset_of!(DisplayListCommandRun, ink_bounds);
    Ok(DisplayListCommandRun {
        offset: u32_at(offset_of!(DisplayListCommandRun, offset)),
        size: u32_at(offset_of!(DisplayListCommandRun, size)),
        context: ContextRef {
            spatial: SpatialNodeIndex(u32_at(context + offset_of!(ContextRef, spatial))),
            clip: ClipNodeIndex(u32_at(context + offset_of!(ContextRef, clip))),
            effect: EffectNodeIndex(u32_at(context + offset_of!(ContextRef, effect))),
        },
        ink_bounds: IntRect::new(
            i32_at(ink_bounds + offset_of!(IntRect, x)),
            i32_at(ink_bounds + offset_of!(IntRect, y)),
            i32_at(ink_bounds + offset_of!(IntRect, width)),
            i32_at(ink_bounds + offset_of!(IntRect, height)),
        ),
        has_unbounded_draw: bool_at(offset_of!(DisplayListCommandRun, has_unbounded_draw))?,
        has_compositor_metadata: bool_at(offset_of!(DisplayListCommandRun, has_compositor_metadata))?,
    })
}

/// Checks a tape and the raw bytes of its run table. Runs must start at zero, follow each other without gaps, stay
/// aligned, end at the tape's end, and summarize their records the way the builder does. Returns the runs.
pub fn validate_tape(tape: &[u8], run_bytes: &[u8]) -> Result<Vec<DisplayListCommandRun>, ValidationError> {
    if !tape.len().is_multiple_of(COMMAND_ALIGNMENT) || u32::try_from(tape.len()).is_err() {
        return Err("Display list tape has an invalid size");
    }
    if !run_bytes.len().is_multiple_of(RUN_SIZE) {
        return Err("Display list run table has an invalid size");
    }
    let mut runs = Vec::with_capacity(run_bytes.len() / RUN_SIZE);
    let mut pending = Vec::new();
    let mut next_offset = 0;
    for run_record in run_bytes.as_chunks::<RUN_SIZE>().0 {
        let run = read_run(run_record)?;
        let size = run.size as usize;
        if run.offset as usize != next_offset || size == 0 || !size.is_multiple_of(COMMAND_ALIGNMENT) {
            return Err("Display list command runs do not cover the tape");
        }
        if size > tape.len() - next_offset {
            return Err("Display list command runs exceed the tape");
        }
        if runs
            .last()
            .is_some_and(|previous: &DisplayListCommandRun| previous.context == run.context)
        {
            return Err("Adjacent display list command runs share a visual context");
        }
        let mut summary = RunSummary::default();
        validate_records(&tape[next_offset..next_offset + size], &mut pending, Some(&mut summary))?;
        while let Some(records) = pending.pop() {
            validate_records(records, &mut pending, None)?;
        }
        if !summary.matches(&run) {
            return Err("Display list command run summary disagrees with its records");
        }
        next_offset += size;
        runs.push(run);
    }
    if next_offset != tape.len() {
        return Err("Display list command runs do not cover the tape");
    }
    Ok(runs)
}
