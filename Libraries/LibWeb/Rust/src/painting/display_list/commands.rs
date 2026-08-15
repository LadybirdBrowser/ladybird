/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::ffi_bytes_fields;
use crate::ffi_enum_bytes;
use crate::painting::display_list::ffi_bytes::FfiBytes;
use libgfx_rust::*;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[repr(u8)]
pub enum DisplayListCommandType {
    DrawGlyphRun,
    FillRect,
    DrawScaledDecodedImageFrame,
    DrawRepeatedDecodedImageFrame,
    DrawRepeatedDisplayList,
    DrawTiledDecodedImageFrame,
    DrawCompositedContext,
    DrawCanvas,
    DrawVideoFrame,
    Save,
    SaveLayer,
    Restore,
    AddClipRect,
    AddClipPath,
    PaintLinearGradient,
    PaintRadialGradient,
    PaintConicGradient,
    PaintOuterBoxShadow,
    PaintInnerBoxShadow,
    PaintTextShadow,
    FillRectWithRoundedCorners,
    FillPath,
    StrokePath,
    DrawEllipse,
    DrawLine,
    ApplyBackdropFilter,
    DrawRect,
    AddRoundedRectClip,
    PaintNestedDisplayList,
    CompositorScrollNode,
    CompositorStickyArea,
    CompositorWheelHitTestTarget,
    CompositorWheelHitTestTargetWithCornerRadii,
    CompositorMainThreadWheelEventRegion,
    CompositorViewportScrollbar,
    CompositorBlockingWheelEventRegion,
    PaintScrollBar,
    ApplyEffects,
}
ffi_enum_bytes!(DisplayListCommandType as u8);

pub const DISPLAY_LIST_COMMAND_TYPE_COUNT: usize = DisplayListCommandType::ApplyEffects as usize + 1;

impl DisplayListCommandType {
    pub fn from_u8(value: u8) -> Option<Self> {
        if (value as usize) < DISPLAY_LIST_COMMAND_TYPE_COUNT {
            // SAFETY: the enum is a dense u8 range starting at zero.
            Some(unsafe { std::mem::transmute::<u8, Self>(value) })
        } else {
            None
        }
    }

    pub const fn is_compositor_metadata(self) -> bool {
        matches!(
            self,
            Self::CompositorScrollNode
                | Self::CompositorStickyArea
                | Self::CompositorWheelHitTestTarget
                | Self::CompositorWheelHitTestTargetWithCornerRadii
                | Self::CompositorMainThreadWheelEventRegion
                | Self::CompositorViewportScrollbar
                | Self::CompositorBlockingWheelEventRegion
        )
    }

    pub const fn nesting_level_change(self) -> i32 {
        match self {
            Self::Save | Self::SaveLayer | Self::ApplyEffects => 1,
            Self::Restore => -1,
            _ => 0,
        }
    }

    pub const fn name(self) -> &'static str {
        match self {
            Self::DrawGlyphRun => "DrawGlyphRun",
            Self::FillRect => "FillRect",
            Self::DrawScaledDecodedImageFrame => "DrawScaledDecodedImageFrame",
            Self::DrawRepeatedDecodedImageFrame => "DrawRepeatedDecodedImageFrame",
            Self::DrawRepeatedDisplayList => "DrawRepeatedDisplayList",
            Self::DrawTiledDecodedImageFrame => "DrawTiledDecodedImageFrame",
            Self::DrawCompositedContext => "DrawCompositedContext",
            Self::DrawCanvas => "DrawCanvas",
            Self::DrawVideoFrame => "DrawVideoFrame",
            Self::Save => "Save",
            Self::SaveLayer => "SaveLayer",
            Self::Restore => "Restore",
            Self::AddClipRect => "AddClipRect",
            Self::AddClipPath => "AddClipPath",
            Self::PaintLinearGradient => "PaintLinearGradient",
            Self::PaintRadialGradient => "PaintRadialGradient",
            Self::PaintConicGradient => "PaintConicGradient",
            Self::PaintOuterBoxShadow => "PaintOuterBoxShadow",
            Self::PaintInnerBoxShadow => "PaintInnerBoxShadow",
            Self::PaintTextShadow => "PaintTextShadow",
            Self::FillRectWithRoundedCorners => "FillRectWithRoundedCorners",
            Self::FillPath => "FillPath",
            Self::StrokePath => "StrokePath",
            Self::DrawEllipse => "DrawEllipse",
            Self::DrawLine => "DrawLine",
            Self::ApplyBackdropFilter => "ApplyBackdropFilter",
            Self::DrawRect => "DrawRect",
            Self::AddRoundedRectClip => "AddRoundedRectClip",
            Self::PaintNestedDisplayList => "PaintNestedDisplayList",
            Self::CompositorScrollNode => "CompositorScrollNode",
            Self::CompositorStickyArea => "CompositorStickyArea",
            Self::CompositorWheelHitTestTarget => "CompositorWheelHitTestTarget",
            Self::CompositorWheelHitTestTargetWithCornerRadii => "CompositorWheelHitTestTargetWithCornerRadii",
            Self::CompositorMainThreadWheelEventRegion => "CompositorMainThreadWheelEventRegion",
            Self::CompositorViewportScrollbar => "CompositorViewportScrollbar",
            Self::CompositorBlockingWheelEventRegion => "CompositorBlockingWheelEventRegion",
            Self::PaintScrollBar => "PaintScrollBar",
            Self::ApplyEffects => "ApplyEffects",
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[repr(transparent)]
pub struct VisualContextIndex(pub usize);

impl FfiBytes for VisualContextIndex {
    #[inline]
    fn write_ffi_bytes(&self, out: &mut [u8]) {
        self.0.write_ffi_bytes(out);
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[repr(transparent)]
pub struct FontResourceId(pub u64);

impl FfiBytes for FontResourceId {
    #[inline]
    fn write_ffi_bytes(&self, out: &mut [u8]) {
        self.0.write_ffi_bytes(out);
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[repr(transparent)]
pub struct ImageFrameResourceId(pub u64);

impl FfiBytes for ImageFrameResourceId {
    #[inline]
    fn write_ffi_bytes(&self, out: &mut [u8]) {
        self.0.write_ffi_bytes(out);
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[repr(transparent)]
pub struct VideoSinkResourceId(pub u64);

impl FfiBytes for VideoSinkResourceId {
    #[inline]
    fn write_ffi_bytes(&self, out: &mut [u8]) {
        self.0.write_ffi_bytes(out);
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[repr(transparent)]
pub struct DisplayListResourceId(pub u64);

impl FfiBytes for DisplayListResourceId {
    #[inline]
    fn write_ffi_bytes(&self, out: &mut [u8]) {
        self.0.write_ffi_bytes(out);
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[repr(transparent)]
pub struct CanvasId(pub u64);

impl FfiBytes for CanvasId {
    #[inline]
    fn write_ffi_bytes(&self, out: &mut [u8]) {
        self.0.write_ffi_bytes(out);
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[repr(transparent)]
pub struct CompositorContextId(pub u64);

impl FfiBytes for CompositorContextId {
    #[inline]
    fn write_ffi_bytes(&self, out: &mut [u8]) {
        self.0.write_ffi_bytes(out);
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[repr(transparent)]
pub struct UniqueNodeId(pub i64);

impl FfiBytes for UniqueNodeId {
    #[inline]
    fn write_ffi_bytes(&self, out: &mut [u8]) {
        self.0.write_ffi_bytes(out);
    }
}

pub const VISUAL_VIEWPORT_NODE_INDEX: VisualContextIndex = VisualContextIndex(0);

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct OptionalFloatRect {
    pub value: FloatRect,
    pub has_value: bool,
}
ffi_bytes_fields!(OptionalFloatRect { value, has_value });

impl OptionalFloatRect {
    pub const fn none() -> Self {
        Self {
            value: FloatRect {
                x: 0.0,
                y: 0.0,
                width: 0.0,
                height: 0.0,
            },
            has_value: false,
        }
    }
    pub const fn some(value: FloatRect) -> Self {
        Self { value, has_value: true }
    }
    pub fn get(self) -> Option<FloatRect> {
        self.has_value.then_some(self.value)
    }
}

impl From<Option<FloatRect>> for OptionalFloatRect {
    fn from(value: Option<FloatRect>) -> Self {
        match value {
            Some(value) => Self::some(value),
            None => Self::none(),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct OptionalColor {
    pub value: Color,
    pub has_value: bool,
}
ffi_bytes_fields!(OptionalColor { value, has_value });

impl OptionalColor {
    pub const fn none() -> Self {
        Self {
            value: Color(0),
            has_value: false,
        }
    }
    pub const fn some(value: Color) -> Self {
        Self { value, has_value: true }
    }
    pub fn get(self) -> Option<Color> {
        self.has_value.then_some(self.value)
    }
}

impl From<Option<Color>> for OptionalColor {
    fn from(value: Option<Color>) -> Self {
        match value {
            Some(value) => Self::some(value),
            None => Self::none(),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct OptionalU32 {
    pub value: u32,
    pub has_value: bool,
}
ffi_bytes_fields!(OptionalU32 { value, has_value });

impl OptionalU32 {
    pub const fn none() -> Self {
        Self {
            value: 0,
            has_value: false,
        }
    }
    pub const fn some(value: u32) -> Self {
        Self { value, has_value: true }
    }
    pub fn get(self) -> Option<u32> {
        self.has_value.then_some(self.value)
    }
}

impl From<Option<u32>> for OptionalU32 {
    fn from(value: Option<u32>) -> Self {
        match value {
            Some(value) => Self::some(value),
            None => Self::none(),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct OptionalF32 {
    pub value: f32,
    pub has_value: bool,
}
ffi_bytes_fields!(OptionalF32 { value, has_value });

impl OptionalF32 {
    pub const fn none() -> Self {
        Self {
            value: 0.0,
            has_value: false,
        }
    }
    pub const fn some(value: f32) -> Self {
        Self { value, has_value: true }
    }
    pub fn get(self) -> Option<f32> {
        self.has_value.then_some(self.value)
    }
}

impl From<Option<f32>> for OptionalF32 {
    fn from(value: Option<f32>) -> Self {
        match value {
            Some(value) => Self::some(value),
            None => Self::none(),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct OptionalAffineTransform {
    pub value: AffineTransform,
    pub has_value: bool,
}
ffi_bytes_fields!(OptionalAffineTransform { value, has_value });

impl OptionalAffineTransform {
    pub const fn none() -> Self {
        Self {
            value: AffineTransform { values: [0.0; 6] },
            has_value: false,
        }
    }
    pub const fn some(value: AffineTransform) -> Self {
        Self { value, has_value: true }
    }
    pub fn get(self) -> Option<AffineTransform> {
        self.has_value.then_some(self.value)
    }
}

impl From<Option<AffineTransform>> for OptionalAffineTransform {
    fn from(value: Option<AffineTransform>) -> Self {
        match value {
            Some(value) => Self::some(value),
            None => Self::none(),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct DisplayListDataSpan {
    // Offset into the command payload containing this span.
    pub offset: u32,
    pub size: u32,
}
ffi_bytes_fields!(DisplayListDataSpan { offset, size });

impl DisplayListDataSpan {
    pub const fn is_empty(self) -> bool {
        self.size == 0
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct DisplayListGradientColorStops {
    pub colors: DisplayListDataSpan,
    pub positions: DisplayListDataSpan,
    pub repeating: bool,
}
ffi_bytes_fields!(DisplayListGradientColorStops {
    colors,
    positions,
    repeating
});

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct DisplayListCommandHeader {
    pub command_type: DisplayListCommandType,
    pub payload_size: u32,
    pub context_index: VisualContextIndex,
    // Apply only the coordinate-affecting nodes of the context chain, skipping clips and effects. Used
    // by inspector overlays, which track the highlighted element's transforms and scroll offsets but
    // must not be clipped or faded by its ancestors.
    pub context_geometry_only: bool,
    pub has_bounding_rect: bool,
    pub is_clip: bool,
    pub bounding_rect: IntRect,
}
ffi_bytes_fields!(DisplayListCommandHeader {
    command_type,
    payload_size,
    context_index,
    context_geometry_only,
    has_bounding_rect,
    is_clip,
    bounding_rect
});

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct DisplayListGlyph {
    pub position: FloatPoint,
    pub glyph_id: u32,
}
ffi_bytes_fields!(DisplayListGlyph { position, glyph_id });

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum CompositorScrollNodeKind {
    Viewport,
    #[default]
    Element,
    PseudoElement,
}
ffi_enum_bytes!(CompositorScrollNodeKind as u8);

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum PathPaintKind {
    #[default]
    Color,
    PaintStyle,
}
ffi_enum_bytes!(PathPaintKind as u8);

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum DisplayListPaintStyleType {
    #[default]
    None,
    LinearGradient,
    RadialGradient,
    Pattern,
}
ffi_enum_bytes!(DisplayListPaintStyleType as u8);

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum DisplayListGradientSpreadMethod {
    #[default]
    Pad,
    Repeat,
    Reflect,
}
ffi_enum_bytes!(DisplayListGradientSpreadMethod as u8);

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct DisplayListGradientPaintStyle {
    pub gradient_transform: OptionalAffineTransform,
    pub spread_method: DisplayListGradientSpreadMethod,
    pub color_space: InterpolationColorSpace,
    pub color_stops: DisplayListGradientColorStops,
}
ffi_bytes_fields!(DisplayListGradientPaintStyle {
    gradient_transform,
    spread_method,
    color_space,
    color_stops
});

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct DisplayListPaintStyle {
    pub paint_style_type: DisplayListPaintStyleType,
    pub gradient: DisplayListGradientPaintStyle,
    pub linear_gradient_start_point: FloatPoint,
    pub linear_gradient_end_point: FloatPoint,
    pub radial_gradient_start_center: FloatPoint,
    pub radial_gradient_start_radius: f32,
    pub radial_gradient_end_center: FloatPoint,
    pub radial_gradient_end_radius: f32,
    pub pattern_tile_display_list_id: DisplayListResourceId,
    pub pattern_tile_rect: FloatRect,
    pub pattern_content_scale: FloatSize,
    pub pattern_transform: OptionalAffineTransform,
}
ffi_bytes_fields!(DisplayListPaintStyle {
    paint_style_type,
    gradient,
    linear_gradient_start_point,
    linear_gradient_end_point,
    radial_gradient_start_center,
    radial_gradient_start_radius,
    radial_gradient_end_center,
    radial_gradient_end_radius,
    pattern_tile_display_list_id,
    pattern_tile_rect,
    pattern_content_scale,
    pattern_transform
});

impl Default for DisplayListPaintStyle {
    fn default() -> Self {
        Self {
            paint_style_type: DisplayListPaintStyleType::None,
            gradient: DisplayListGradientPaintStyle::default(),
            linear_gradient_start_point: FloatPoint::default(),
            linear_gradient_end_point: FloatPoint::default(),
            radial_gradient_start_center: FloatPoint::default(),
            radial_gradient_start_radius: 0.0,
            radial_gradient_end_center: FloatPoint::default(),
            radial_gradient_end_radius: 0.0,
            pattern_tile_display_list_id: DisplayListResourceId(0),
            pattern_tile_rect: FloatRect::default(),
            pattern_content_scale: FloatSize {
                width: 1.0,
                height: 1.0,
            },
            pattern_transform: OptionalAffineTransform::none(),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct Repeat {
    pub x: bool,
    pub y: bool,
}
ffi_bytes_fields!(Repeat { x, y });

pub trait DisplayListCommand: Copy + FfiBytes {
    const COMMAND_TYPE: DisplayListCommandType;
    fn bounding_rect(&self) -> Option<IntRect> {
        None
    }
    fn is_clip(&self) -> bool {
        false
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct DrawGlyphRun {
    pub font_id: FontResourceId,
    pub glyphs: DisplayListDataSpan,
    pub rect: IntRect,
    pub glyph_bounding_rect: IntRect,
    pub translation: FloatPoint,
    pub scale: f32,
    pub color: Color,
    pub orientation: Orientation,
}
ffi_bytes_fields!(DrawGlyphRun {
    font_id,
    glyphs,
    rect,
    glyph_bounding_rect,
    translation,
    scale,
    color,
    orientation
});

impl DisplayListCommand for DrawGlyphRun {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::DrawGlyphRun;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.glyph_bounding_rect)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct FillRect {
    pub rect: IntRect,
    pub color: Color,
}
ffi_bytes_fields!(FillRect { rect, color });

impl DisplayListCommand for FillRect {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::FillRect;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.rect)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct DrawScaledDecodedImageFrame {
    pub dst_rect: FloatRect,
    pub src_rect: OptionalFloatRect,
    pub frame_id: ImageFrameResourceId,
    pub scaling_mode: ScalingMode,
    pub compositing_and_blending_operator: CompositingAndBlendingOperator,
    pub isolated_backdrop_color: OptionalColor,
}
ffi_bytes_fields!(DrawScaledDecodedImageFrame {
    dst_rect,
    src_rect,
    frame_id,
    scaling_mode,
    compositing_and_blending_operator,
    isolated_backdrop_color
});

impl DisplayListCommand for DrawScaledDecodedImageFrame {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::DrawScaledDecodedImageFrame;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(enclosing_int_rect(self.dst_rect))
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct DrawRepeatedDecodedImageFrame {
    pub dst_rect: IntRect,
    pub clip_rect: IntRect,
    pub frame_id: ImageFrameResourceId,
    pub scaling_mode: ScalingMode,
    pub repeat: Repeat,
    pub compositing_and_blending_operator: CompositingAndBlendingOperator,
    pub isolated_backdrop_color: OptionalColor,
}
ffi_bytes_fields!(DrawRepeatedDecodedImageFrame {
    dst_rect,
    clip_rect,
    frame_id,
    scaling_mode,
    repeat,
    compositing_and_blending_operator,
    isolated_backdrop_color
});

impl DisplayListCommand for DrawRepeatedDecodedImageFrame {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::DrawRepeatedDecodedImageFrame;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.clip_rect)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct DrawRepeatedDisplayList {
    pub dst_rect: IntRect,
    pub clip_rect: IntRect,
    pub display_list_id: DisplayListResourceId,
    pub scaling_mode: ScalingMode,
    pub repeat: Repeat,
}
ffi_bytes_fields!(DrawRepeatedDisplayList {
    dst_rect,
    clip_rect,
    display_list_id,
    scaling_mode,
    repeat
});

impl DisplayListCommand for DrawRepeatedDisplayList {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::DrawRepeatedDisplayList;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.clip_rect)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct DrawTiledDecodedImageFrame {
    pub tile_rect: FloatRect,
    pub clip_rect: IntRect,
    pub src_rect: FloatRect,
    pub tile_step: FloatSize,
    pub frame_id: ImageFrameResourceId,
    pub scaling_mode: ScalingMode,
    pub tile_count_x: OptionalU32,
    pub tile_count_y: OptionalU32,
}
ffi_bytes_fields!(DrawTiledDecodedImageFrame {
    tile_rect,
    clip_rect,
    src_rect,
    tile_step,
    frame_id,
    scaling_mode,
    tile_count_x,
    tile_count_y
});

impl DisplayListCommand for DrawTiledDecodedImageFrame {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::DrawTiledDecodedImageFrame;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.clip_rect)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct DrawCompositedContext {
    pub dst_rect: IntRect,
    pub child_context_id: CompositorContextId,
    pub scaling_mode: ScalingMode,
}
ffi_bytes_fields!(DrawCompositedContext {
    dst_rect,
    child_context_id,
    scaling_mode
});

impl DisplayListCommand for DrawCompositedContext {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::DrawCompositedContext;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.dst_rect)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct DrawCanvas {
    pub dst_rect: IntRect,
    pub canvas_id: CanvasId,
    // NB: The canvas pixels live in the compositor's canvas surface registry, so the command bytes don't
    //     change when the canvas content does. The content generation encodes content changes so that display
    //     list damage computation can tell that the canvas needs to be repainted.
    pub content_generation: u64,
    pub scaling_mode: ScalingMode,
}
ffi_bytes_fields!(DrawCanvas {
    dst_rect,
    canvas_id,
    content_generation,
    scaling_mode
});

impl DisplayListCommand for DrawCanvas {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::DrawCanvas;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.dst_rect)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct DrawVideoFrame {
    pub dst_rect: IntRect,
    pub video_sink_id: VideoSinkResourceId,
    pub scaling_mode: ScalingMode,
}
ffi_bytes_fields!(DrawVideoFrame {
    dst_rect,
    video_sink_id,
    scaling_mode
});

impl DisplayListCommand for DrawVideoFrame {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::DrawVideoFrame;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.dst_rect)
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct Save {
    _empty: u8,
}
ffi_bytes_fields!(Save { _empty });

impl DisplayListCommand for Save {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::Save;
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct SaveLayer {
    _empty: u8,
}
ffi_bytes_fields!(SaveLayer { _empty });

impl DisplayListCommand for SaveLayer {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::SaveLayer;
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct Restore {
    _empty: u8,
}
ffi_bytes_fields!(Restore { _empty });

impl DisplayListCommand for Restore {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::Restore;
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct AddClipRect {
    pub rect: FloatRect,
}
ffi_bytes_fields!(AddClipRect { rect });

impl DisplayListCommand for AddClipRect {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::AddClipRect;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(enclosing_int_rect(self.rect))
    }
    fn is_clip(&self) -> bool {
        true
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct AddClipPath {
    pub path_bounding_rect: IntRect,
    pub path_data: DisplayListDataSpan,
    pub winding_rule: WindingRule,
}
ffi_bytes_fields!(AddClipPath {
    path_bounding_rect,
    path_data,
    winding_rule
});

impl DisplayListCommand for AddClipPath {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::AddClipPath;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.path_bounding_rect)
    }
    fn is_clip(&self) -> bool {
        true
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct PaintLinearGradient {
    pub gradient_rect: IntRect,
    pub gradient_angle: f32,
    pub color_stops: DisplayListGradientColorStops,
    pub first_stop_position: f32,
    pub repeat_length: f32,
    pub interpolation_method: GradientInterpolationMethod,
}
ffi_bytes_fields!(PaintLinearGradient {
    gradient_rect,
    gradient_angle,
    color_stops,
    first_stop_position,
    repeat_length,
    interpolation_method
});

impl DisplayListCommand for PaintLinearGradient {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::PaintLinearGradient;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.gradient_rect)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct PaintOuterBoxShadow {
    pub color: Color,
    pub blur_radius: i32,
    pub device_content_rect: IntRect,
    pub content_corner_radii: CornerRadii,
    pub shadow_rect: IntRect,
    pub shadow_corner_radii: CornerRadii,
}
ffi_bytes_fields!(PaintOuterBoxShadow {
    color,
    blur_radius,
    device_content_rect,
    content_corner_radii,
    shadow_rect,
    shadow_corner_radii
});

impl DisplayListCommand for PaintOuterBoxShadow {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::PaintOuterBoxShadow;
    fn bounding_rect(&self) -> Option<IntRect> {
        let blur_margin = self.blur_radius * 2;
        Some(
            self.shadow_rect
                .inflated_edges(blur_margin, blur_margin, blur_margin, blur_margin),
        )
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct PaintInnerBoxShadow {
    pub color: Color,
    pub blur_radius: i32,
    pub device_content_rect: IntRect,
    pub content_corner_radii: CornerRadii,
    pub outer_shadow_rect: IntRect,
    pub inner_shadow_rect: IntRect,
    pub inner_shadow_corner_radii: CornerRadii,
}
ffi_bytes_fields!(PaintInnerBoxShadow {
    color,
    blur_radius,
    device_content_rect,
    content_corner_radii,
    outer_shadow_rect,
    inner_shadow_rect,
    inner_shadow_corner_radii
});

impl DisplayListCommand for PaintInnerBoxShadow {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::PaintInnerBoxShadow;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.device_content_rect)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct PaintTextShadow {
    pub font_id: FontResourceId,
    pub glyphs: DisplayListDataSpan,
    pub shadow_bounding_rect: IntRect,
    pub text_rect: IntRect,
    pub draw_location: FloatPoint,
    pub scale: f32,
    pub blur_radius: i32,
    pub color: Color,
}
ffi_bytes_fields!(PaintTextShadow {
    font_id,
    glyphs,
    shadow_bounding_rect,
    text_rect,
    draw_location,
    scale,
    blur_radius,
    color
});

impl DisplayListCommand for PaintTextShadow {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::PaintTextShadow;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(IntRect::new(
            self.draw_location.x as i32,
            self.draw_location.y as i32,
            self.shadow_bounding_rect.width,
            self.shadow_bounding_rect.height,
        ))
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct FillRectWithRoundedCorners {
    pub rect: IntRect,
    pub color: Color,
    pub corner_radii: CornerRadii,
}
ffi_bytes_fields!(FillRectWithRoundedCorners {
    rect,
    color,
    corner_radii
});

impl DisplayListCommand for FillRectWithRoundedCorners {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::FillRectWithRoundedCorners;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.rect)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct FillPath {
    pub path_bounding_rect: FloatRect,
    pub path_data: DisplayListDataSpan,
    pub opacity: f32,
    pub paint_kind: PathPaintKind,
    pub color: Color,
    pub paint_style: DisplayListPaintStyle,
    pub winding_rule: WindingRule,
    pub should_anti_alias: ShouldAntiAlias,
}
ffi_bytes_fields!(FillPath {
    path_bounding_rect,
    path_data,
    opacity,
    paint_kind,
    color,
    paint_style,
    winding_rule,
    should_anti_alias
});

impl DisplayListCommand for FillPath {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::FillPath;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(enclosing_int_rect(self.path_bounding_rect))
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct StrokePath {
    pub cap_style: CapStyle,
    pub join_style: JoinStyle,
    pub miter_limit: f32,
    pub dash_array: DisplayListDataSpan,
    pub dash_offset: f32,
    pub path_bounding_rect: FloatRect,
    pub path_data: DisplayListDataSpan,
    pub opacity: f32,
    pub paint_kind: PathPaintKind,
    pub color: Color,
    pub paint_style: DisplayListPaintStyle,
    pub thickness: f32,
    pub should_anti_alias: ShouldAntiAlias,
}
ffi_bytes_fields!(StrokePath {
    cap_style,
    join_style,
    miter_limit,
    dash_array,
    dash_offset,
    path_bounding_rect,
    path_data,
    opacity,
    paint_kind,
    color,
    paint_style,
    thickness,
    should_anti_alias
});

impl DisplayListCommand for StrokePath {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::StrokePath;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(enclosing_int_rect(self.path_bounding_rect))
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct DrawEllipse {
    pub rect: IntRect,
    pub color: Color,
    pub thickness: i32,
}
ffi_bytes_fields!(DrawEllipse { rect, color, thickness });

impl DisplayListCommand for DrawEllipse {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::DrawEllipse;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.rect)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct DrawLine {
    pub color: Color,
    pub from: IntPoint,
    pub to: IntPoint,
    pub thickness: i32,
    pub style: LineStyle,
    pub alternate_color: Color,
}
ffi_bytes_fields!(DrawLine {
    color,
    from,
    to,
    thickness,
    style,
    alternate_color
});

impl DisplayListCommand for DrawLine {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::DrawLine;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(int_rect_from_two_points(self.from, self.to).inflated(self.thickness, self.thickness))
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct ApplyBackdropFilter {
    pub backdrop_region: IntRect,
    pub corner_radii: CornerRadii,
    pub has_backdrop_filter: bool,
    pub backdrop_filter_data: DisplayListDataSpan,
}
ffi_bytes_fields!(ApplyBackdropFilter {
    backdrop_region,
    corner_radii,
    has_backdrop_filter,
    backdrop_filter_data
});

impl DisplayListCommand for ApplyBackdropFilter {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::ApplyBackdropFilter;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.backdrop_region)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct DrawRect {
    pub rect: IntRect,
    pub color: Color,
    pub rough: bool,
}
ffi_bytes_fields!(DrawRect { rect, color, rough });

impl DisplayListCommand for DrawRect {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::DrawRect;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.rect)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct PaintRadialGradient {
    pub rect: IntRect,
    pub color_stops: DisplayListGradientColorStops,
    pub interpolation_method: GradientInterpolationMethod,
    pub center: IntPoint,
    pub size: IntSize,
}
ffi_bytes_fields!(PaintRadialGradient {
    rect,
    color_stops,
    interpolation_method,
    center,
    size
});

impl DisplayListCommand for PaintRadialGradient {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::PaintRadialGradient;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.rect)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct PaintConicGradient {
    pub rect: IntRect,
    pub start_angle: f32,
    pub color_stops: DisplayListGradientColorStops,
    pub interpolation_method: GradientInterpolationMethod,
    pub position: IntPoint,
}
ffi_bytes_fields!(PaintConicGradient {
    rect,
    start_angle,
    color_stops,
    interpolation_method,
    position
});

impl DisplayListCommand for PaintConicGradient {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::PaintConicGradient;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.rect)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct AddRoundedRectClip {
    pub corner_radii: CornerRadii,
    pub border_rect: IntRect,
    pub corner_clip: CornerClip,
}
ffi_bytes_fields!(AddRoundedRectClip {
    corner_radii,
    border_rect,
    corner_clip
});

impl DisplayListCommand for AddRoundedRectClip {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::AddRoundedRectClip;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.border_rect)
    }
    fn is_clip(&self) -> bool {
        true
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct PaintNestedDisplayList {
    pub display_list_id: DisplayListResourceId,
    pub rect: FloatRect,
    // The size the nested list was recorded at; replay scales it into the destination rect.
    pub list_size: IntSize,
}
ffi_bytes_fields!(PaintNestedDisplayList {
    display_list_id,
    rect,
    list_size
});

impl DisplayListCommand for PaintNestedDisplayList {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::PaintNestedDisplayList;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(enclosing_int_rect(self.rect))
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct CompositorScrollNode {
    pub document_id: UniqueNodeId,
    pub scrollable_node_id: UniqueNodeId,
    pub scroll_node_index: VisualContextIndex,
    pub parent_scroll_node_index: VisualContextIndex,
    pub scrollport_rect: IntRect,
    pub min_scroll_offset: FloatPoint,
    pub max_scroll_offset: FloatPoint,
    pub scroll_node_kind: CompositorScrollNodeKind,
    pub pseudo_element_type: u8,
    pub is_viewport: bool,
    pub can_be_wheel_scrolled_horizontally: bool,
    pub can_be_wheel_scrolled_vertically: bool,
}
ffi_bytes_fields!(CompositorScrollNode {
    document_id,
    scrollable_node_id,
    scroll_node_index,
    parent_scroll_node_index,
    scrollport_rect,
    min_scroll_offset,
    max_scroll_offset,
    scroll_node_kind,
    pseudo_element_type,
    is_viewport,
    can_be_wheel_scrolled_horizontally,
    can_be_wheel_scrolled_vertically
});

impl DisplayListCommand for CompositorScrollNode {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::CompositorScrollNode;
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct CompositorStickyArea {
    pub document_id: UniqueNodeId,
    pub scroll_node_index: VisualContextIndex,
    pub parent_scroll_node_index: VisualContextIndex,
    pub nearest_scrolling_ancestor_index: VisualContextIndex,
    pub position_relative_to_scroll_ancestor: FloatPoint,
    pub border_box_size: FloatSize,
    pub scrollport_size: FloatSize,
    pub containing_block_region: FloatRect,
    pub needs_parent_offset_adjustment: bool,
    pub inset_top: OptionalF32,
    pub inset_right: OptionalF32,
    pub inset_bottom: OptionalF32,
    pub inset_left: OptionalF32,
}
ffi_bytes_fields!(CompositorStickyArea {
    document_id,
    scroll_node_index,
    parent_scroll_node_index,
    nearest_scrolling_ancestor_index,
    position_relative_to_scroll_ancestor,
    border_box_size,
    scrollport_size,
    containing_block_region,
    needs_parent_offset_adjustment,
    inset_top,
    inset_right,
    inset_bottom,
    inset_left
});

impl DisplayListCommand for CompositorStickyArea {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::CompositorStickyArea;
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct CompositorBlockingWheelEventRegion {
    pub rect: FloatRect,
}
ffi_bytes_fields!(CompositorBlockingWheelEventRegion { rect });

impl DisplayListCommand for CompositorBlockingWheelEventRegion {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::CompositorBlockingWheelEventRegion;
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct CompositorWheelHitTestTarget {
    pub document_id: UniqueNodeId,
    pub target_scroll_node_index: VisualContextIndex,
    pub rect: FloatRect,
}
ffi_bytes_fields!(CompositorWheelHitTestTarget {
    document_id,
    target_scroll_node_index,
    rect
});

impl DisplayListCommand for CompositorWheelHitTestTarget {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::CompositorWheelHitTestTarget;
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct CompositorWheelHitTestTargetWithCornerRadii {
    pub document_id: UniqueNodeId,
    pub target_scroll_node_index: VisualContextIndex,
    pub rect: FloatRect,
    pub corner_radii: CornerRadii,
}
ffi_bytes_fields!(CompositorWheelHitTestTargetWithCornerRadii {
    document_id,
    target_scroll_node_index,
    rect,
    corner_radii
});

impl DisplayListCommand for CompositorWheelHitTestTargetWithCornerRadii {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::CompositorWheelHitTestTargetWithCornerRadii;
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct CompositorMainThreadWheelEventRegion {
    pub rect: FloatRect,
}
ffi_bytes_fields!(CompositorMainThreadWheelEventRegion { rect });

impl DisplayListCommand for CompositorMainThreadWheelEventRegion {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::CompositorMainThreadWheelEventRegion;
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct CompositorViewportScrollbar {
    pub document_id: UniqueNodeId,
    pub scroll_node_index: VisualContextIndex,
    pub gutter_rect: IntRect,
    pub thumb_rect: IntRect,
    pub expanded_gutter_rect: IntRect,
    pub expanded_thumb_rect: IntRect,
    pub scroll_size: f64,
    pub expanded_scroll_size: f64,
    pub min_scroll_offset: f32,
    pub max_scroll_offset: f32,
    pub thumb_color: Color,
    pub track_color: Color,
    pub vertical: bool,
}
ffi_bytes_fields!(CompositorViewportScrollbar {
    document_id,
    scroll_node_index,
    gutter_rect,
    thumb_rect,
    expanded_gutter_rect,
    expanded_thumb_rect,
    scroll_size,
    expanded_scroll_size,
    min_scroll_offset,
    max_scroll_offset,
    thumb_color,
    track_color,
    vertical
});

impl DisplayListCommand for CompositorViewportScrollbar {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::CompositorViewportScrollbar;
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct PaintScrollBar {
    pub scroll_node_index: VisualContextIndex,
    pub gutter_rect: IntRect,
    pub thumb_rect: IntRect,
    pub track_rect: IntRect,
    pub scroll_size: f64,
    pub thumb_color: Color,
    pub track_color: Color,
    pub vertical: bool,
}
ffi_bytes_fields!(PaintScrollBar {
    scroll_node_index,
    gutter_rect,
    thumb_rect,
    track_rect,
    scroll_size,
    thumb_color,
    track_color,
    vertical
});

impl DisplayListCommand for PaintScrollBar {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::PaintScrollBar;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.track_rect.united(self.thumb_rect))
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct ApplyEffects {
    pub opacity: f32,
    pub compositing_and_blending_operator: CompositingAndBlendingOperator,
    pub has_filter: bool,
    pub filter_data: DisplayListDataSpan,
    pub has_mask_kind: bool,
    pub mask_kind: MaskKind,
}
ffi_bytes_fields!(ApplyEffects {
    opacity,
    compositing_and_blending_operator,
    has_filter,
    filter_data,
    has_mask_kind,
    mask_kind
});

impl DisplayListCommand for ApplyEffects {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::ApplyEffects;
}

pub fn int_rect_from_two_points(a: IntPoint, b: IntPoint) -> IntRect {
    IntRect::new(a.x.min(b.x), a.y.min(b.y), (a.x - b.x).abs(), (a.y - b.y).abs())
}
