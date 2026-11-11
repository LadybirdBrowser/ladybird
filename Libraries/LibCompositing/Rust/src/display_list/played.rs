/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The commands as a painter plays them: a copy of each command, and what its payload holds, already read. Arrays come
//! as spans and paths as their contours, so a painter never reads a payload.

use super::builder::read_command;
use super::commands::*;
use super::nested_records::span_bytes;
use ak::Span;
use libgfx_rust::path::Path;
use libgfx_rust::path::ffi::PathGeometryData;
use libgfx_rust::Color;
use std::borrow::Cow;
use std::ffi::c_void;

/// # Safety
///
/// The type has no padding, and every bit pattern is a valid value of it.
unsafe trait PlainValue: Copy {}

// SAFETY: Two floats and a u32.
unsafe impl PlainValue for DisplayListGlyph {}
// SAFETY: Two floats, three i32s and a color.
unsafe impl PlainValue for TextShadowLayer {}
// SAFETY: A u32.
unsafe impl PlainValue for Color {}
// SAFETY: A float.
unsafe impl PlainValue for f32 {}
// SAFETY: Two u64s.
unsafe impl PlainValue for Canvas2DGlyphRun {}

// The values of an array in a payload, which validation sized to a whole number of them. They are read in place when
// the payload is aligned for them, and copied otherwise.
fn values_of<T: PlainValue>(bytes: &[u8]) -> Cow<'_, [T]> {
    let count = bytes.len() / size_of::<T>();
    let values = bytes.as_ptr().cast::<T>();
    if values.is_aligned() {
        // SAFETY: The bytes hold `count` aligned values, and any bits are a valid `T`.
        Cow::Borrowed(unsafe { ak::slice_from_raw_parts(values, count) })
    } else {
        // SAFETY: The bytes hold `count` values, and any bits are a valid `T`.
        Cow::Owned(
            (0..count)
                .map(|index| unsafe { values.add(index).read_unaligned() })
                .collect(),
        )
    }
}

#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct PlayedGradientColorStops<'a> {
    pub colors: Span<'a, Color>,
    pub positions: Span<'a, f32>,
}

/// What the payload of a command holds, read. A command leaves empty what it has none of: its glyphs, the layers of a
/// text shadow, the glyph runs of a 2D canvas, the stops of a gradient or of a gradient paint, its dash array, its
/// path, which is null for none, and the serialized filter graph of a 2D canvas draw.
#[repr(C)]
pub struct PlayedPayload<'a> {
    pub glyphs: Span<'a, DisplayListGlyph>,
    pub text_shadow_layers: Span<'a, TextShadowLayer>,
    pub glyph_runs: Span<'a, Canvas2DGlyphRun>,
    pub color_stops: PlayedGradientColorStops<'a>,
    pub dash_array: Span<'a, f32>,
    pub path: *const PathGeometryData,
    pub filter: Span<'a, u8>,
}

/// The values a played payload lends.
#[derive(Default)]
pub(crate) struct PayloadValues<'p> {
    glyphs: Cow<'p, [DisplayListGlyph]>,
    text_shadow_layers: Cow<'p, [TextShadowLayer]>,
    glyph_runs: Cow<'p, [Canvas2DGlyphRun]>,
    colors: Cow<'p, [Color]>,
    positions: Cow<'p, [f32]>,
    dash_array: Cow<'p, [f32]>,
    path: Option<(Path, PathGeometryData)>,
    filter: &'p [u8],
}

impl<'p> PayloadValues<'p> {
    pub(crate) fn read(command: &PlayedCommand, payload: &'p [u8]) -> Self {
        let mut values = Self::default();
        let bytes = |span| span_bytes(payload, span);
        match command {
            PlayedCommand::DrawGlyphRun(command) => values.glyphs = values_of(bytes(command.glyphs)),
            PlayedCommand::PaintLinearGradient(command) => values.read_color_stops(payload, command.color_stops),
            PlayedCommand::PaintRadialGradient(command) => values.read_color_stops(payload, command.color_stops),
            PlayedCommand::PaintConicGradient(command) => values.read_color_stops(payload, command.color_stops),
            PlayedCommand::PaintPathBoxShadow(command) => values.read_path(bytes(command.shape_path_data)),
            PlayedCommand::PaintTextShadow(command) => {
                values.glyphs = values_of(bytes(command.glyphs));
                values.text_shadow_layers = values_of(bytes(command.layers));
            }
            PlayedCommand::FillPath(command) => {
                values.read_path(bytes(command.path_data));
                values.read_paint_style(payload, command.paint_kind, &command.paint_style);
            }
            PlayedCommand::StrokePath(command) => {
                values.read_path(bytes(command.path_data));
                values.dash_array = values_of(bytes(command.dash_array));
                values.read_paint_style(payload, command.paint_kind, &command.paint_style);
            }
            PlayedCommand::Canvas2DFillPath(command) => {
                values.read_path(bytes(command.path_data));
                values.read_canvas_2d_compositing(payload, Some(&command.paint), &command.compositing);
            }
            PlayedCommand::Canvas2DStrokePath(command) => {
                values.read_path(bytes(command.path_data));
                values.dash_array = values_of(bytes(command.dash_array));
                values.read_canvas_2d_compositing(payload, Some(&command.paint), &command.compositing);
            }
            PlayedCommand::Canvas2DDrawGlyphRuns(command) => {
                values.glyph_runs = values_of(bytes(command.runs));
                values.glyphs = values_of(bytes(command.glyphs));
                values.read_canvas_2d_compositing(payload, Some(&command.paint), &command.compositing);
            }
            PlayedCommand::Canvas2DDrawImageFrame(command) => {
                values.read_canvas_2d_compositing(payload, None, &command.compositing);
            }
            PlayedCommand::Canvas2DDrawCanvas(command) => {
                values.read_canvas_2d_compositing(payload, None, &command.compositing);
            }
            PlayedCommand::Canvas2DDrawVideoFrame(command) => {
                values.read_canvas_2d_compositing(payload, None, &command.compositing);
            }
            PlayedCommand::Canvas2DClipPath(command) => values.read_path(bytes(command.path_data)),
            _ => {}
        }
        values
    }

    pub(crate) fn played(&self) -> PlayedPayload<'_> {
        PlayedPayload {
            glyphs: Span::new(&self.glyphs),
            text_shadow_layers: Span::new(&self.text_shadow_layers),
            glyph_runs: Span::new(&self.glyph_runs),
            color_stops: PlayedGradientColorStops {
                colors: Span::new(&self.colors),
                positions: Span::new(&self.positions),
            },
            dash_array: Span::new(&self.dash_array),
            path: self.path.as_ref().map_or(std::ptr::null(), |(_, data)| data),
            filter: Span::new(self.filter),
        }
    }

    fn read_path(&mut self, bytes: &[u8]) {
        let path = Path::from_serialized_bytes(bytes);
        let data = path.geometry_data();
        self.path = Some((path, data));
    }

    fn read_color_stops(&mut self, payload: &'p [u8], color_stops: DisplayListGradientColorStops) {
        self.colors = values_of(span_bytes(payload, color_stops.colors));
        self.positions = values_of(span_bytes(payload, color_stops.positions));
    }

    fn read_paint_style(&mut self, payload: &'p [u8], kind: PathPaintKind, style: &DisplayListPaintStyle) {
        if kind == PathPaintKind::PaintStyle
            && matches!(
                style.paint_style_type,
                DisplayListPaintStyleType::LinearGradient | DisplayListPaintStyleType::RadialGradient
            )
        {
            self.read_color_stops(payload, style.gradient.color_stops);
        }
    }

    fn read_canvas_2d_compositing(
        &mut self,
        payload: &'p [u8],
        paint: Option<&Canvas2DPaint>,
        compositing: &Canvas2DCompositing,
    ) {
        if let Some(paint) = paint
            && matches!(
                paint.kind,
                Canvas2DPaintKind::LinearGradient
                    | Canvas2DPaintKind::RadialGradient
                    | Canvas2DPaintKind::ConicGradient
            )
        {
            self.read_color_stops(payload, paint.color_stops);
        }
        if compositing.has_filter {
            self.filter = span_bytes(payload, compositing.filter);
        }
    }
}

// The build script reads this table to generate the commands the C++ painters dispatch on.
macro_rules! played_commands {
    (played: [$($played:ident,)*], never_played: [$($never:ident,)*],) => {
        /// A copy of a command a painter plays.
        pub enum PlayedCommand {
            $($played($played),)*
        }

        impl PlayedCommand {
            /// Copies the command at the start of `payload`.
            pub fn read(command_type: DisplayListCommandType, payload: &[u8]) -> Self {
                match command_type {
                    $(DisplayListCommandType::$played => Self::$played(read_command::<$played>(payload)),)*
                    $(DisplayListCommandType::$never => unreachable!(concat!(stringify!($never), " is never played as a command")),)*
                }
            }

            pub fn command_type(&self) -> DisplayListCommandType {
                match self {
                    $(Self::$played(_) => DisplayListCommandType::$played,)*
                }
            }

            /// The address of the command for a C++ painter, which the command type tells the type of.
            pub fn as_ffi_pointer(&self) -> *const c_void {
                match self {
                    $(Self::$played(command) => std::ptr::from_ref(command).cast(),)*
                }
            }
        }
    };
}

played_commands! {
    played: [
        FillRect, PaintCaret, DrawScaledDecodedImageFrame, DrawRepeatedDecodedImageFrame, DrawRepeatedTile,
        DrawTiledDecodedImageFrame, DrawCompositedContext, DrawCanvas, DrawVideoFrame, PaintOuterBoxShadow,
        PaintInnerBoxShadow, FillRectWithRoundedCorners, FillRoundedRectRing, DrawEllipse, DrawLine, DrawRect,
        PaintNestedDisplayList, PaintScrollBar, Canvas2DClearRect, Canvas2DSetTransform, Canvas2DSave, Canvas2DRestore,
        Canvas2DReset, Canvas2DClearCanvas, Canvas2DPutImageFrame, DrawGlyphRun, PaintLinearGradient,
        PaintRadialGradient, PaintConicGradient, PaintPathBoxShadow, PaintTextShadow, FillPath, StrokePath,
        Canvas2DFillPath, Canvas2DStrokePath, Canvas2DDrawGlyphRuns, Canvas2DDrawImageFrame, Canvas2DDrawCanvas,
        Canvas2DDrawVideoFrame, Canvas2DClipPath,
    ],
    never_played: [
        BackdropFilterRegion, DrawIsolatedGroup, DeclareMaskContent, CompositorWheelHitTestTarget,
        CompositorWheelHitTestTargetWithCornerRadii, CompositorMainThreadWheelEventRegion, CompositorScrollbar,
        CompositorBlockingWheelEventRegion, CompositorNestedContextWheelRegion,
    ],
}
