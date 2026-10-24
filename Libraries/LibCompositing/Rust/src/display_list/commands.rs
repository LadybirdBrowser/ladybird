/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css_pixels::{FfiCssPixelPoint, FfiCssPixelRect};
use crate::display_list::ffi_bytes::FfiBytes;
use crate::ffi_bytes_fields;
use crate::ffi_enum_bytes;
use libgfx_rust::*;

// The layout of records and runs on the tape, which only Rust reads.
pub use super::tape::*;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[repr(u8)]
pub enum DisplayListCommandType {
    DrawGlyphRun,
    FillRect,
    PaintCaret,
    DrawScaledDecodedImageFrame,
    DrawRepeatedDecodedImageFrame,
    DrawRepeatedTile,
    DrawTiledDecodedImageFrame,
    DrawCompositedContext,
    DrawCanvas,
    DrawVideoFrame,
    PaintLinearGradient,
    PaintRadialGradient,
    PaintConicGradient,
    PaintOuterBoxShadow,
    PaintInnerBoxShadow,
    PaintPathBoxShadow,
    PaintTextShadow,
    FillRectWithRoundedCorners,
    FillRoundedRectRing,
    FillPath,
    StrokePath,
    DrawEllipse,
    DrawLine,
    BackdropFilterRegion,
    DrawRect,
    PaintNestedDisplayList,
    DrawIsolatedGroup,
    DeclareMaskContent,
    CompositorScrollNode,
    CompositorWheelHitTestTarget,
    CompositorWheelHitTestTargetWithCornerRadii,
    CompositorMainThreadWheelEventRegion,
    CompositorScrollbar,
    CompositorBlockingWheelEventRegion,
    PaintScrollBar,
    CompositorSnapContainer,
    CompositorSnapArea,
    CompositorScrollChainingBarrier,
    CompositorNestedContextWheelRegion,
    Canvas2DFillPath,
    Canvas2DStrokePath,
    Canvas2DDrawGlyphRun,
    Canvas2DClearRect,
    Canvas2DDrawImageFrame,
    Canvas2DDrawCanvas,
    Canvas2DDrawVideoFrame,
    Canvas2DSetTransform,
    Canvas2DSave,
    Canvas2DRestore,
    Canvas2DClipPath,
    Canvas2DReset,
    Canvas2DClearCanvas,
    Canvas2DPutImageFrame,
}
ffi_enum_bytes!(DisplayListCommandType as u8);

pub const DISPLAY_LIST_COMMAND_TYPE_COUNT: usize = DisplayListCommandType::Canvas2DPutImageFrame as usize + 1;

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
                | Self::CompositorWheelHitTestTarget
                | Self::CompositorWheelHitTestTargetWithCornerRadii
                | Self::CompositorMainThreadWheelEventRegion
                | Self::CompositorScrollbar
                | Self::CompositorBlockingWheelEventRegion
                | Self::CompositorSnapContainer
                | Self::CompositorSnapArea
                | Self::CompositorScrollChainingBarrier
                | Self::CompositorNestedContextWheelRegion
        )
    }

    /// A command of the stream of a 2D canvas, which the canvas replays against its own surface in recording order,
    /// under the state that the commands before it saved, transformed, clipped and restored. Such a command draws no
    /// known rect, and the display list of a page never holds one. Its name starts with "Canvas2D", which is how the
    /// build script tells it apart.
    pub const fn is_canvas_2d(self) -> bool {
        matches!(
            self,
            Self::Canvas2DFillPath
                | Self::Canvas2DStrokePath
                | Self::Canvas2DDrawGlyphRun
                | Self::Canvas2DClearRect
                | Self::Canvas2DDrawImageFrame
                | Self::Canvas2DDrawCanvas
                | Self::Canvas2DDrawVideoFrame
                | Self::Canvas2DSetTransform
                | Self::Canvas2DSave
                | Self::Canvas2DRestore
                | Self::Canvas2DClipPath
                | Self::Canvas2DReset
                | Self::Canvas2DClearCanvas
                | Self::Canvas2DPutImageFrame
        )
    }

    pub const fn name(self) -> &'static str {
        match self {
            Self::DrawGlyphRun => "DrawGlyphRun",
            Self::FillRect => "FillRect",
            Self::PaintCaret => "PaintCaret",
            Self::DrawScaledDecodedImageFrame => "DrawScaledDecodedImageFrame",
            Self::DrawRepeatedDecodedImageFrame => "DrawRepeatedDecodedImageFrame",
            Self::DrawRepeatedTile => "DrawRepeatedTile",
            Self::DrawTiledDecodedImageFrame => "DrawTiledDecodedImageFrame",
            Self::DrawCompositedContext => "DrawCompositedContext",
            Self::DrawCanvas => "DrawCanvas",
            Self::DrawVideoFrame => "DrawVideoFrame",
            Self::PaintLinearGradient => "PaintLinearGradient",
            Self::PaintRadialGradient => "PaintRadialGradient",
            Self::PaintConicGradient => "PaintConicGradient",
            Self::PaintOuterBoxShadow => "PaintOuterBoxShadow",
            Self::PaintInnerBoxShadow => "PaintInnerBoxShadow",
            Self::PaintPathBoxShadow => "PaintPathBoxShadow",
            Self::PaintTextShadow => "PaintTextShadow",
            Self::FillRectWithRoundedCorners => "FillRectWithRoundedCorners",
            Self::FillRoundedRectRing => "FillRoundedRectRing",
            Self::FillPath => "FillPath",
            Self::StrokePath => "StrokePath",
            Self::DrawEllipse => "DrawEllipse",
            Self::DrawLine => "DrawLine",
            Self::BackdropFilterRegion => "BackdropFilterRegion",
            Self::DrawRect => "DrawRect",
            Self::PaintNestedDisplayList => "PaintNestedDisplayList",
            Self::DrawIsolatedGroup => "DrawIsolatedGroup",
            Self::DeclareMaskContent => "DeclareMaskContent",
            Self::CompositorScrollNode => "CompositorScrollNode",
            Self::CompositorWheelHitTestTarget => "CompositorWheelHitTestTarget",
            Self::CompositorWheelHitTestTargetWithCornerRadii => "CompositorWheelHitTestTargetWithCornerRadii",
            Self::CompositorMainThreadWheelEventRegion => "CompositorMainThreadWheelEventRegion",
            Self::CompositorScrollbar => "CompositorScrollbar",
            Self::CompositorBlockingWheelEventRegion => "CompositorBlockingWheelEventRegion",
            Self::PaintScrollBar => "PaintScrollBar",
            Self::CompositorSnapContainer => "CompositorSnapContainer",
            Self::CompositorSnapArea => "CompositorSnapArea",
            Self::CompositorScrollChainingBarrier => "CompositorScrollChainingBarrier",
            Self::CompositorNestedContextWheelRegion => "CompositorNestedContextWheelRegion",
            Self::Canvas2DFillPath => "Canvas2DFillPath",
            Self::Canvas2DStrokePath => "Canvas2DStrokePath",
            Self::Canvas2DDrawGlyphRun => "Canvas2DDrawGlyphRun",
            Self::Canvas2DClearRect => "Canvas2DClearRect",
            Self::Canvas2DDrawImageFrame => "Canvas2DDrawImageFrame",
            Self::Canvas2DDrawCanvas => "Canvas2DDrawCanvas",
            Self::Canvas2DDrawVideoFrame => "Canvas2DDrawVideoFrame",
            Self::Canvas2DSetTransform => "Canvas2DSetTransform",
            Self::Canvas2DSave => "Canvas2DSave",
            Self::Canvas2DRestore => "Canvas2DRestore",
            Self::Canvas2DClipPath => "Canvas2DClipPath",
            Self::Canvas2DReset => "Canvas2DReset",
            Self::Canvas2DClearCanvas => "Canvas2DClearCanvas",
            Self::Canvas2DPutImageFrame => "Canvas2DPutImageFrame",
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[repr(transparent)]
pub struct SpatialNodeIndex(pub u32);

impl FfiBytes for SpatialNodeIndex {
    #[inline]
    fn write_ffi_bytes(&self, out: &mut [u8]) {
        self.0.write_ffi_bytes(out);
    }
}

// Indices into the clip and effect trees. `u32::MAX` is the absent node.
macro_rules! optional_node_index {
    ($name:ident) => {
        #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
        #[repr(transparent)]
        pub struct $name(pub u32);

        impl $name {
            pub const NONE: Self = Self(u32::MAX);

            pub const fn is_none(self) -> bool {
                self.0 == u32::MAX
            }
        }

        impl Default for $name {
            fn default() -> Self {
                Self::NONE
            }
        }

        impl FfiBytes for $name {
            #[inline]
            fn write_ffi_bytes(&self, out: &mut [u8]) {
                self.0.write_ffi_bytes(out);
            }
        }
    };
}

optional_node_index!(ClipNodeIndex);
optional_node_index!(EffectNodeIndex);

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
#[repr(C)]
pub struct ContextRef {
    pub spatial: SpatialNodeIndex,
    pub clip: ClipNodeIndex,
    pub effect: EffectNodeIndex,
}
ffi_bytes_fields!(ContextRef { spatial, clip, effect });

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum ClipMode {
    Intersect,
    Difference,
}
ffi_enum_bytes!(ClipMode as u8);

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct ReplayClip {
    pub rect: FloatRect,
    pub corner_radii: CornerRadii,
    pub mode: ClipMode,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct ReplayLayer {
    pub opacity: f32,
    pub blend_mode: CompositingAndBlendingOperator,
    pub filter_bytes: *const u8,
    pub filter_bytes_size: usize,
    pub backdrop_filter_bytes: *const u8,
    pub backdrop_filter_bytes_size: usize,
    pub backdrop_region: IntRect,
    pub backdrop_corner_radii: CornerRadii,
    pub effect: EffectNodeIndex,
    /// Bounds of everything replay draws into the layer, in the effect's space, when known.
    pub content_bounds: OptionalFloatRect,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct ReplayMask {
    pub rect: IntRect,
    pub kind: MaskKind,
}

impl ContextRef {
    pub const fn spatial_only(spatial: SpatialNodeIndex) -> Self {
        Self {
            spatial,
            clip: ClipNodeIndex::NONE,
            effect: EffectNodeIndex::NONE,
        }
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

pub const VISUAL_VIEWPORT_NODE_INDEX: SpatialNodeIndex = SpatialNodeIndex(0);

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
    pub pattern_tile: DisplayListDataSpan,
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
    pattern_tile,
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
            pattern_tile: DisplayListDataSpan::default(),
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
    // Encloses everything the command draws. Only compositor metadata, which draws nothing, and the commands of the
    // canvas 2D stream, which draw under state that only replay knows, have none.
    fn bounding_rect(&self) -> Option<IntRect> {
        const {
            assert!(
                Self::COMMAND_TYPE.is_compositor_metadata() || Self::COMMAND_TYPE.is_canvas_2d(),
                "a draw command must report its bounding rect"
            );
        };
        None
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
    pub font_smoothing: u8,
}
ffi_bytes_fields!(DrawGlyphRun {
    font_id,
    glyphs,
    rect,
    glyph_bounding_rect,
    translation,
    scale,
    color,
    orientation,
    font_smoothing
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
    pub compositing_and_blending_operator: CompositingAndBlendingOperator,
    pub background_color_animation_effect: EffectNodeIndex,
}
ffi_bytes_fields!(FillRect {
    rect,
    color,
    compositing_and_blending_operator,
    background_color_animation_effect
});

impl DisplayListCommand for FillRect {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::FillRect;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.rect)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct PaintCaret {
    pub rect: IntRect,
    pub color: Color,
    pub blink_cycle_start_time_ns: i64,
    pub should_blink: bool,
}
ffi_bytes_fields!(PaintCaret {
    rect,
    color,
    blink_cycle_start_time_ns,
    should_blink
});

impl DisplayListCommand for PaintCaret {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::PaintCaret;
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
    pub apply_force_dark: bool,
}
ffi_bytes_fields!(DrawScaledDecodedImageFrame {
    dst_rect,
    src_rect,
    frame_id,
    scaling_mode,
    compositing_and_blending_operator,
    isolated_backdrop_color,
    apply_force_dark
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
    pub apply_force_dark: bool,
}
ffi_bytes_fields!(DrawRepeatedDecodedImageFrame {
    dst_rect,
    clip_rect,
    frame_id,
    scaling_mode,
    repeat,
    compositing_and_blending_operator,
    isolated_backdrop_color,
    apply_force_dark
});

impl DisplayListCommand for DrawRepeatedDecodedImageFrame {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::DrawRepeatedDecodedImageFrame;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.clip_rect)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct DrawRepeatedTile {
    pub dst_rect: FloatRect,
    pub clip_rect: IntRect,
    pub tile_size: IntSize,
    pub tile_step: FloatSize,
    // Tile records paint into a raster of tile_size, with its origin at (0, 0).
    pub tile: DisplayListDataSpan,
    pub scaling_mode: ScalingMode,
    pub compositing_and_blending_operator: CompositingAndBlendingOperator,
    pub repeat: Repeat,
}
ffi_bytes_fields!(DrawRepeatedTile {
    dst_rect,
    clip_rect,
    tile_size,
    tile_step,
    tile,
    scaling_mode,
    compositing_and_blending_operator,
    repeat
});

impl DisplayListCommand for DrawRepeatedTile {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::DrawRepeatedTile;
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
    pub apply_force_dark: bool,
}
ffi_bytes_fields!(DrawTiledDecodedImageFrame {
    tile_rect,
    clip_rect,
    src_rect,
    tile_step,
    frame_id,
    scaling_mode,
    tile_count_x,
    tile_count_y,
    apply_force_dark
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
    pub dst_rect: FloatRect,
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
        Some(enclosing_int_rect(self.dst_rect))
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

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct PaintLinearGradient {
    pub gradient_rect: IntRect,
    pub gradient_angle: f32,
    pub color_stops: DisplayListGradientColorStops,
    pub first_stop_position: f32,
    pub repeat_length: f32,
    pub interpolation_method: GradientInterpolationMethod,
    pub compositing_and_blending_operator: CompositingAndBlendingOperator,
}
ffi_bytes_fields!(PaintLinearGradient {
    gradient_rect,
    gradient_angle,
    color_stops,
    first_stop_position,
    repeat_length,
    interpolation_method,
    compositing_and_blending_operator
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

/// A box shadow of a box whose shape is a path: an outer shadow drawn outside the shape, or an inner one drawn inside
/// it. The shadow's shape is the box's shape grown by the spread distance for an outer shadow, or what lies outside
/// the box's shape shrunk by it for an inner one, moved by the offset.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct PaintPathBoxShadow {
    pub color: Color,
    pub blur_radius: i32,
    pub spread_distance: f32,
    pub offset: FloatPoint,
    pub shape_path_bounding_rect: FloatRect,
    pub shape_path_data: DisplayListDataSpan,
    pub inner: bool,
}
ffi_bytes_fields!(PaintPathBoxShadow {
    color,
    blur_radius,
    spread_distance,
    offset,
    shape_path_bounding_rect,
    shape_path_data,
    inner
});

impl DisplayListCommand for PaintPathBoxShadow {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::PaintPathBoxShadow;
    fn bounding_rect(&self) -> Option<IntRect> {
        let shape_rect = enclosing_int_rect(self.shape_path_bounding_rect);
        if self.inner {
            return Some(shape_rect);
        }
        // NB: The player grows the shape by the outline of a miter-joined stroke, whose miters reach out to Skia's default
        //     miter limit of 4 times half the stroke width, so up to 4 times the spread at sharp corners.
        const MITER_LIMIT: f32 = 4.0;
        let margin = (self.spread_distance.max(0.0) * MITER_LIMIT).ceil() as i32 + self.blur_radius * 2;
        let mut shadow_rect = shape_rect.inflated_edges(margin, margin, margin, margin);
        shadow_rect.x += self.offset.x.floor() as i32;
        shadow_rect.y += self.offset.y.floor() as i32;
        shadow_rect.width += 1;
        shadow_rect.height += 1;
        Some(shadow_rect)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct TextShadowLayer {
    // Added to the glyph run's translation.
    pub offset: FloatPoint,
    // Added to the glyph run's rect, which is the rotation pivot of vertical text.
    pub rounded_offset: IntPoint,
    pub blur_radius: i32,
    pub color: Color,
}
ffi_bytes_fields!(TextShadowLayer {
    offset,
    rounded_offset,
    blur_radius,
    color
});

// Every text-shadow layer of one glyph run, in painting order (the bottom-most layer first).
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct PaintTextShadow {
    pub font_id: FontResourceId,
    pub glyphs: DisplayListDataSpan,
    pub layers: DisplayListDataSpan,
    pub shadows_bounding_rect: IntRect,
    pub rect: IntRect,
    pub translation: FloatPoint,
    pub scale: f32,
    pub orientation: Orientation,
    pub font_smoothing: u8,
}
ffi_bytes_fields!(PaintTextShadow {
    font_id,
    glyphs,
    layers,
    shadows_bounding_rect,
    rect,
    translation,
    scale,
    orientation,
    font_smoothing
});

impl DisplayListCommand for PaintTextShadow {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::PaintTextShadow;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.shadows_bounding_rect)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct FillRectWithRoundedCorners {
    pub rect: IntRect,
    pub color: Color,
    pub corner_radii: CornerRadii,
    pub background_color_animation_effect: EffectNodeIndex,
}
ffi_bytes_fields!(FillRectWithRoundedCorners {
    rect,
    color,
    corner_radii,
    background_color_animation_effect
});

impl DisplayListCommand for FillRectWithRoundedCorners {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::FillRectWithRoundedCorners;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.rect)
    }
}

/// The band between a rounded rect and the same rect shrunk by an edge width on each side, with each corner
/// of the inner outline keeping the outer radius minus the widths of its two adjacent edges (square once
/// either reaches zero). This is the shape of a uniformly colored CSS border, painted in one draw.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct FillRoundedRectRing {
    pub rect: IntRect,
    pub corner_radii: CornerRadii,
    pub top_width: i32,
    pub right_width: i32,
    pub bottom_width: i32,
    pub left_width: i32,
    pub color: Color,
}
ffi_bytes_fields!(FillRoundedRectRing {
    rect,
    corner_radii,
    top_width,
    right_width,
    bottom_width,
    left_width,
    color
});

impl DisplayListCommand for FillRoundedRectRing {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::FillRoundedRectRing;
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
    pub compositing_and_blending_operator: CompositingAndBlendingOperator,
}
ffi_bytes_fields!(FillPath {
    path_bounding_rect,
    path_data,
    opacity,
    paint_kind,
    color,
    paint_style,
    winding_rule,
    should_anti_alias,
    compositing_and_blending_operator
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
    // The path bounding rect reaches half the thickness past the path, as far as round and bevel
    // joins and butt and round caps paint. Miter joins reach out to the miter limit times that, and
    // the corners of square caps reach out diagonally.
    fn bounding_rect(&self) -> Option<IntRect> {
        let mut reach_multiplier: f32 = 1.0;
        if self.join_style == JoinStyle::Miter {
            reach_multiplier = reach_multiplier.max(self.miter_limit);
        }
        if self.cap_style == CapStyle::Square {
            reach_multiplier = reach_multiplier.max(std::f32::consts::SQRT_2);
        }
        let reach_past_path_bounding_rect = self.thickness / 2.0 * (reach_multiplier - 1.0);
        Some(enclosing_int_rect(self.path_bounding_rect.inflated(
            2.0 * reach_past_path_bounding_rect,
            2.0 * reach_past_path_bounding_rect,
        )))
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

// The backdrop filter is applied by the effect the box records under. This command holds the box's
// place in paint order, so the effect is entered there even when the box paints nothing else.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct BackdropFilterRegion {
    pub rect: IntRect,
}
ffi_bytes_fields!(BackdropFilterRegion { rect });

impl DisplayListCommand for BackdropFilterRegion {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::BackdropFilterRegion;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.rect)
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
    pub compositing_and_blending_operator: CompositingAndBlendingOperator,
}
ffi_bytes_fields!(PaintRadialGradient {
    rect,
    color_stops,
    interpolation_method,
    center,
    size,
    compositing_and_blending_operator
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
    pub compositing_and_blending_operator: CompositingAndBlendingOperator,
}
ffi_bytes_fields!(PaintConicGradient {
    rect,
    start_angle,
    color_stops,
    interpolation_method,
    position,
    compositing_and_blending_operator
});

impl DisplayListCommand for PaintConicGradient {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::PaintConicGradient;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.rect)
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

// Plays the content records inside an internally scoped saveLayer that carries
// the group's opacity, blend and filter, optionally masked by a second run of
// records composited with DestinationIn, so a group that must not blend with
// the canvas needs no clips or effects. Both runs are recorded in place inside
// this command's payload under its context.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct DrawIsolatedGroup {
    pub clip_rect: FloatRect,
    pub content: DisplayListDataSpan,
    pub mask: DisplayListDataSpan,
    pub filter: DisplayListDataSpan,
    pub opacity: f32,
    pub compositing_and_blending_operator: CompositingAndBlendingOperator,
    pub mask_kind: MaskKind,
}
ffi_bytes_fields!(DrawIsolatedGroup {
    clip_rect,
    content,
    mask,
    filter,
    opacity,
    compositing_and_blending_operator,
    mask_kind
});

impl DisplayListCommand for DrawIsolatedGroup {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::DrawIsolatedGroup;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(enclosing_int_rect(self.clip_rect))
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct DeclareMaskContent {
    pub rect: IntRect,
    pub effect: EffectNodeIndex,
    pub content: DisplayListDataSpan,
}
ffi_bytes_fields!(DeclareMaskContent { rect, effect, content });

impl DisplayListCommand for DeclareMaskContent {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::DeclareMaskContent;
    fn bounding_rect(&self) -> Option<IntRect> {
        Some(self.rect)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct CompositorScrollNode {
    pub document_id: UniqueNodeId,
    pub scrollable_node_id: UniqueNodeId,
    pub scroll_node_index: SpatialNodeIndex,
    pub parent_scroll_node_index: SpatialNodeIndex,
    pub scrollport_rect: IntRect,
    pub min_scroll_offset: FloatPoint,
    pub max_scroll_offset: FloatPoint,
    pub scroll_node_kind: CompositorScrollNodeKind,
    pub pseudo_element_type: u8,
    pub is_viewport: bool,
    pub can_be_wheel_scrolled_horizontally: bool,
    pub can_be_wheel_scrolled_vertically: bool,
    /// Whether a scroll the scroller cannot scroll by is passed on to the scrollers around it.
    pub chains_scroll_to_ancestors_horizontally: bool,
    pub chains_scroll_to_ancestors_vertically: bool,
    /// Whether a touchpad may pull the scroller past the edges of its scroll range.
    pub shows_overscroll_effect_horizontally: bool,
    pub shows_overscroll_effect_vertically: bool,
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
    can_be_wheel_scrolled_vertically,
    chains_scroll_to_ancestors_horizontally,
    chains_scroll_to_ancestors_vertically,
    shows_overscroll_effect_horizontally,
    shows_overscroll_effect_vertically
});

impl DisplayListCommand for CompositorScrollNode {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::CompositorScrollNode;
}

/// The geometry snap position selection runs over for a scroll node that is a snap container. In CSS
/// pixels so that the compositor selects the same position the main thread would.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct CompositorSnapContainer {
    pub document_id: UniqueNodeId,
    pub scroll_node_index: SpatialNodeIndex,
    pub snapport: FfiCssPixelRect,
    pub min_scroll_offset: FfiCssPixelPoint,
    pub max_scroll_offset: FfiCssPixelPoint,
    pub strictness: u8,
    pub snaps_x: bool,
    pub snaps_y: bool,
    pub horizontal_writing_mode: bool,
}
ffi_bytes_fields!(CompositorSnapContainer {
    document_id,
    scroll_node_index,
    snapport,
    min_scroll_offset,
    max_scroll_offset,
    strictness,
    snaps_x,
    snaps_y,
    horizontal_writing_mode
});

impl DisplayListCommand for CompositorSnapContainer {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::CompositorSnapContainer;
}

/// A snap area of the snap container recorded before it: the transformed border box with the scroll
/// margin added, in the container's coordinate space, and the alignment along each physical axis.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct CompositorSnapArea {
    pub document_id: UniqueNodeId,
    pub scroll_node_index: SpatialNodeIndex,
    pub area_node_id: UniqueNodeId,
    pub pseudo_element_type: u8,
    pub rect: FfiCssPixelRect,
    pub align_x: u8,
    pub align_y: u8,
    pub always_stop: bool,
}
ffi_bytes_fields!(CompositorSnapArea {
    document_id,
    scroll_node_index,
    area_node_id,
    pseudo_element_type,
    rect,
    align_x,
    align_y,
    always_stop
});

impl DisplayListCommand for CompositorSnapArea {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::CompositorSnapArea;
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
    pub target_scroll_node_index: SpatialNodeIndex,
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
    pub target_scroll_node_index: SpatialNodeIndex,
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

/// A scroll container that has no scroll node, because a wheel can scroll nothing in it, and that keeps the scrolls
/// along an axis to itself all the same: a wheel inside `rect` is not passed on to the scroll node above it.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct CompositorScrollChainingBarrier {
    pub document_id: UniqueNodeId,
    pub scroll_node_index_above_barrier: SpatialNodeIndex,
    pub rect: FloatRect,
    pub keeps_horizontal_scrolls: bool,
    pub keeps_vertical_scrolls: bool,
}
ffi_bytes_fields!(CompositorScrollChainingBarrier {
    document_id,
    scroll_node_index_above_barrier,
    rect,
    keeps_horizontal_scrolls,
    keeps_vertical_scrolls
});

impl DisplayListCommand for CompositorScrollChainingBarrier {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::CompositorScrollChainingBarrier;
}

/// Where a nested context is painted: its content box, clipped to the rounded padding edge. The compositor can hand a
/// wheel inside `rect` that nothing painted later covers to the nested context's own scroll nodes.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct CompositorNestedContextWheelRegion {
    pub rect: FloatRect,
    pub corner_radii: CornerRadii,
    pub child_context_id: CompositorContextId,
}
ffi_bytes_fields!(CompositorNestedContextWheelRegion {
    rect,
    corner_radii,
    child_context_id
});

impl DisplayListCommand for CompositorNestedContextWheelRegion {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::CompositorNestedContextWheelRegion;
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
pub struct CompositorScrollbar {
    pub document_id: UniqueNodeId,
    pub scroll_node_index: SpatialNodeIndex,
    pub gutter_rect: IntRect,
    pub thumb_rect: IntRect,
    pub track_rect: IntRect,
    pub expanded_gutter_rect: IntRect,
    pub expanded_thumb_rect: IntRect,
    // The thumb is clipped to this rounded rect, which follows the rounded corners the UI may give the viewport.
    // A radius of zero means no clip.
    pub corner_clip_rect: IntRect,
    pub scroll_size: f64,
    pub expanded_scroll_size: f64,
    pub min_scroll_offset: f32,
    pub max_scroll_offset: f32,
    pub corner_clip_radius: i32,
    pub thumb_color: Color,
    pub track_color: Color,
    pub vertical: bool,
    pub is_painted_by_compositor: bool,
    pub display_list_paints_enlarged_scrollbar: bool,
}
ffi_bytes_fields!(CompositorScrollbar {
    document_id,
    scroll_node_index,
    gutter_rect,
    thumb_rect,
    track_rect,
    expanded_gutter_rect,
    expanded_thumb_rect,
    corner_clip_rect,
    scroll_size,
    expanded_scroll_size,
    min_scroll_offset,
    max_scroll_offset,
    corner_clip_radius,
    thumb_color,
    track_color,
    vertical,
    is_painted_by_compositor,
    display_list_paints_enlarged_scrollbar
});

impl DisplayListCommand for CompositorScrollbar {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::CompositorScrollbar;
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct PaintScrollBar {
    pub scroll_node_index: SpatialNodeIndex,
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

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum Canvas2DPaintKind {
    #[default]
    Color,
    LinearGradient,
    RadialGradient,
    ConicGradient,
    Pattern,
}
ffi_enum_bytes!(Canvas2DPaintKind as u8);

// The values of Gfx::CanvasPatternPaintStyle::Repetition.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum Canvas2DPatternRepetition {
    #[default]
    Repeat,
    RepeatX,
    RepeatY,
    NoRepeat,
}
ffi_enum_bytes!(Canvas2DPatternRepetition as u8);

/// The fill or stroke style of a 2D canvas draw. Only the fields of its kind are read: the color, the points and the
/// radii of a gradient, which are in the coordinates the draw is transformed from, or the image of a pattern. A
/// pattern without an image is one whose image was not available when script created it.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct Canvas2DPaint {
    pub kind: Canvas2DPaintKind,
    pub pattern_repetition: Canvas2DPatternRepetition,
    pub pattern_has_image: bool,
    pub color: Color,
    pub start_point: FloatPoint,
    pub end_point: FloatPoint,
    pub start_radius: f32,
    pub end_radius: f32,
    pub start_angle: f32,
    pub color_stops: DisplayListGradientColorStops,
    pub pattern_image_id: ImageFrameResourceId,
    pub pattern_transform: OptionalAffineTransform,
}
ffi_bytes_fields!(Canvas2DPaint {
    kind,
    pattern_repetition,
    pattern_has_image,
    color,
    start_point,
    end_point,
    start_radius,
    end_radius,
    start_angle,
    color_stops,
    pattern_image_id,
    pattern_transform
});

/// The shadow that a 2D canvas draw casts, with its offset and blur in the coordinates of the canvas, which the
/// current transformation matrix does not affect.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct Canvas2DShadow {
    pub color: Color,
    pub offset_x: f32,
    pub offset_y: f32,
    pub blur: f32,
}
ffi_bytes_fields!(Canvas2DShadow {
    color,
    offset_x,
    offset_y,
    blur
});

/// How a 2D canvas draw composites onto the canvas: through the serialized filter graph of the context, if it has one,
/// with the global alpha of the context, with its compositing and blending operator, and with the shadow that it
/// casts, if it casts one.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct Canvas2DCompositing {
    pub filter: DisplayListDataSpan,
    pub has_filter: bool,
    pub global_alpha: f32,
    pub compositing_and_blending_operator: CompositingAndBlendingOperator,
    pub has_shadow: bool,
    pub shadow: Canvas2DShadow,
}
ffi_bytes_fields!(Canvas2DCompositing {
    filter,
    has_filter,
    global_alpha,
    compositing_and_blending_operator,
    has_shadow,
    shadow
});

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct Canvas2DFillPath {
    pub path_data: DisplayListDataSpan,
    pub paint: Canvas2DPaint,
    pub winding_rule: WindingRule,
    pub compositing: Canvas2DCompositing,
}
ffi_bytes_fields!(Canvas2DFillPath {
    path_data,
    paint,
    winding_rule,
    compositing
});

impl DisplayListCommand for Canvas2DFillPath {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::Canvas2DFillPath;
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct Canvas2DStrokePath {
    pub path_data: DisplayListDataSpan,
    pub paint: Canvas2DPaint,
    pub thickness: f32,
    pub cap_style: CapStyle,
    pub join_style: JoinStyle,
    pub miter_limit: f32,
    pub dash_array: DisplayListDataSpan,
    pub dash_offset: f32,
    pub compositing: Canvas2DCompositing,
}
ffi_bytes_fields!(Canvas2DStrokePath {
    path_data,
    paint,
    thickness,
    cap_style,
    join_style,
    miter_limit,
    dash_array,
    dash_offset,
    compositing
});

impl DisplayListCommand for Canvas2DStrokePath {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::Canvas2DStrokePath;
}

// The glyphs of one font, positioned relative to the translation, with the top of the line box at the glyph position.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct Canvas2DDrawGlyphRun {
    pub font_id: FontResourceId,
    pub glyphs: DisplayListDataSpan,
    pub translation: FloatPoint,
    pub paint: Canvas2DPaint,
    pub compositing: Canvas2DCompositing,
}
ffi_bytes_fields!(Canvas2DDrawGlyphRun {
    font_id,
    glyphs,
    translation,
    paint,
    compositing
});

impl DisplayListCommand for Canvas2DDrawGlyphRun {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::Canvas2DDrawGlyphRun;
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct Canvas2DClearRect {
    pub rect: FloatRect,
    pub color: Color,
}
ffi_bytes_fields!(Canvas2DClearRect { rect, color });

impl DisplayListCommand for Canvas2DClearRect {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::Canvas2DClearRect;
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct Canvas2DDrawImageFrame {
    pub frame_id: ImageFrameResourceId,
    pub dst_rect: FloatRect,
    pub src_rect: FloatRect,
    pub scaling_mode: ScalingMode,
    pub compositing: Canvas2DCompositing,
}
ffi_bytes_fields!(Canvas2DDrawImageFrame {
    frame_id,
    dst_rect,
    src_rect,
    scaling_mode,
    compositing
});

impl DisplayListCommand for Canvas2DDrawImageFrame {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::Canvas2DDrawImageFrame;
}

// Draws what another canvas of the same connection holds at this point of the stream.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct Canvas2DDrawCanvas {
    pub source_canvas_id: CanvasId,
    pub dst_rect: FloatRect,
    pub src_rect: FloatRect,
    pub scaling_mode: ScalingMode,
    pub compositing: Canvas2DCompositing,
}
ffi_bytes_fields!(Canvas2DDrawCanvas {
    source_canvas_id,
    dst_rect,
    src_rect,
    scaling_mode,
    compositing
});

impl DisplayListCommand for Canvas2DDrawCanvas {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::Canvas2DDrawCanvas;
}

// Draws a frame of a video that WebContent holds for the stream, named by the handle of its sink and its generation.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct Canvas2DDrawVideoFrame {
    pub video_sink_handle: u64,
    pub generation: u64,
    pub dst_rect: FloatRect,
    pub src_rect: FloatRect,
    pub scaling_mode: ScalingMode,
    pub compositing: Canvas2DCompositing,
}
ffi_bytes_fields!(Canvas2DDrawVideoFrame {
    video_sink_handle,
    generation,
    dst_rect,
    src_rect,
    scaling_mode,
    compositing
});

impl DisplayListCommand for Canvas2DDrawVideoFrame {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::Canvas2DDrawVideoFrame;
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct Canvas2DSetTransform {
    pub transform: AffineTransform,
}
ffi_bytes_fields!(Canvas2DSetTransform { transform });

impl DisplayListCommand for Canvas2DSetTransform {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::Canvas2DSetTransform;
}

// NB: C++ gives a struct without fields a size of one byte, so the commands that take no arguments carry padding to
//     be the same size on both sides.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct Canvas2DSave {
    pub padding: u32,
}
ffi_bytes_fields!(Canvas2DSave { padding });

impl DisplayListCommand for Canvas2DSave {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::Canvas2DSave;
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct Canvas2DRestore {
    pub padding: u32,
}
ffi_bytes_fields!(Canvas2DRestore { padding });

impl DisplayListCommand for Canvas2DRestore {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::Canvas2DRestore;
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct Canvas2DClipPath {
    pub path_data: DisplayListDataSpan,
    pub winding_rule: WindingRule,
}
ffi_bytes_fields!(Canvas2DClipPath {
    path_data,
    winding_rule
});

impl DisplayListCommand for Canvas2DClipPath {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::Canvas2DClipPath;
}

// Drops every saved state and clip, back to the state the canvas was created with.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct Canvas2DReset {
    pub padding: u32,
}
ffi_bytes_fields!(Canvas2DReset { padding });

impl DisplayListCommand for Canvas2DReset {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::Canvas2DReset;
}

// Sets every pixel of the canvas to the color, outside of any transform or clip.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct Canvas2DClearCanvas {
    pub color: Color,
}
ffi_bytes_fields!(Canvas2DClearCanvas { color });

impl DisplayListCommand for Canvas2DClearCanvas {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::Canvas2DClearCanvas;
}

// Sets the pixels of the canvas at the position to those of the frame, regardless of the transform, the clip and the
// compositing of the context, as putImageData() does.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct Canvas2DPutImageFrame {
    pub frame_id: ImageFrameResourceId,
    pub position: IntPoint,
}
ffi_bytes_fields!(Canvas2DPutImageFrame { frame_id, position });

impl DisplayListCommand for Canvas2DPutImageFrame {
    const COMMAND_TYPE: DisplayListCommandType = DisplayListCommandType::Canvas2DPutImageFrame;
}

pub fn int_rect_from_two_points(a: IntPoint, b: IntPoint) -> IntRect {
    IntRect::new(a.x.min(b.x), a.y.min(b.y), (a.x - b.x).abs(), (a.y - b.y).abs())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_commands_of_the_canvas_2d_stream_are_the_ones_whose_names_say_so() {
        // The build script sorts the commands into the C++ lists by their names.
        for value in 0..DISPLAY_LIST_COMMAND_TYPE_COUNT {
            let command_type = DisplayListCommandType::from_u8(value as u8).unwrap();
            assert_eq!(
                command_type.is_canvas_2d(),
                command_type.name().starts_with("Canvas2D"),
                "{}",
                command_type.name()
            );
            assert!(!(command_type.is_canvas_2d() && command_type.is_compositor_metadata()));
        }
    }
}
