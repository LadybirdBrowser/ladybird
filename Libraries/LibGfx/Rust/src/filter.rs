/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Image filter graphs and their serialized form.
//!
//! A [`Filter`] is a tree of image filter operations. Painting never executes one itself: filters
//! reach the Skia display list player, the compositor, and IPC as the byte form produced by
//! [`Filter::serialize`], and C++ builds the `SkImageFilter` from that.
//!
//! The byte form is a pre-order walk of the tree. Each node is a one-byte operation tag followed by
//! the operation's fields in declaration order; an optional input is a presence byte followed by
//! the input when present. Scalars are native-endian, enums use the width of their C++ underlying
//! type, colors are their ARGB32 value, and a color table is a `u32` length of 256 followed by the
//! table.

use crate::{
    Color, ColorFilterType, CompositingAndBlendingOperator, FloatRect, IntRect, IntSize, InterpolationColorSpace,
    ScalingMode,
};
use libipc_rust::wire::{Decode, DecodeError, Decoder, Encode, EncodeError, Encoder};

/// Which channel of a displacement map input drives the displacement along one axis.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(i32)]
pub enum ChannelSelector {
    Red,
    Green,
    Blue,
    #[default]
    Alpha,
}

impl ChannelSelector {
    pub fn from_i32(value: i32) -> Option<Self> {
        (Self::Red as i32..=Self::Alpha as i32).contains(&value).then(|| {
            // SAFETY: the enum is a dense i32 range starting at zero.
            unsafe { std::mem::transmute::<i32, Self>(value) }
        })
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(i32)]
pub enum TurbulenceType {
    FractalNoise,
    #[default]
    Turbulence,
}

impl TurbulenceType {
    pub fn from_i32(value: i32) -> Option<Self> {
        (Self::FractalNoise as i32..=Self::Turbulence as i32)
            .contains(&value)
            .then(|| {
                // SAFETY: the enum is a dense i32 range starting at zero.
                unsafe { std::mem::transmute::<i32, Self>(value) }
            })
    }
}

/// The operation tag that starts every serialized filter node.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FilterOperationType {
    Arithmetic,
    Compose,
    Blend,
    Flood,
    DisplacementMap,
    DropShadow,
    Blur,
    ColorFilter,
    ColorMatrix,
    ColorTable,
    Saturate,
    HueRotate,
    Image,
    Merge,
    Offset,
    Erode,
    Dilate,
    Turbulence,
    ColorSpaceConversion,
}

impl FilterOperationType {
    pub fn from_u8(value: u8) -> Option<Self> {
        (value <= Self::ColorSpaceConversion as u8).then(|| {
            // SAFETY: the enum is a dense u8 range starting at zero.
            unsafe { std::mem::transmute::<u8, Self>(value) }
        })
    }
}

/// A 256-entry lookup table for one channel of a component transfer.
pub type ChannelTable = Box<[u8; 256]>;

/// One node of an image filter graph. An absent input means the source graphic.
#[derive(Clone, Debug, PartialEq)]
pub enum Filter {
    Arithmetic {
        background: Option<Box<Filter>>,
        foreground: Option<Box<Filter>>,
        k1: f32,
        k2: f32,
        k3: f32,
        k4: f32,
    },
    Compose {
        outer: Box<Filter>,
        inner: Box<Filter>,
    },
    Blend {
        background: Option<Box<Filter>>,
        foreground: Option<Box<Filter>>,
        mode: CompositingAndBlendingOperator,
    },
    Flood {
        color: Color,
        opacity: f32,
    },
    DisplacementMap {
        color: Option<Box<Filter>>,
        displacement: Option<Box<Filter>>,
        scale: f32,
        x_channel_selector: ChannelSelector,
        y_channel_selector: ChannelSelector,
    },
    DropShadow {
        offset_x: f32,
        offset_y: f32,
        radius: f32,
        color: Color,
        input: Option<Box<Filter>>,
    },
    Blur {
        radius_x: f32,
        radius_y: f32,
        /// Whether this is a blur() filter function, which mirrors at the edge of a backdrop.
        mirrors_at_backdrop_edge: bool,
        input: Option<Box<Filter>>,
    },
    ColorFilter {
        kind: ColorFilterType,
        amount: f32,
        input: Option<Box<Filter>>,
    },
    ColorMatrix {
        matrix: [f32; 20],
        input: Option<Box<Filter>>,
    },
    ColorTable {
        a: Option<ChannelTable>,
        r: Option<ChannelTable>,
        g: Option<ChannelTable>,
        b: Option<ChannelTable>,
        input: Option<Box<Filter>>,
    },
    Saturate {
        value: f32,
        input: Option<Box<Filter>>,
    },
    HueRotate {
        angle_degrees: f32,
        input: Option<Box<Filter>>,
    },
    /// A decoded image frame named by the id it was registered under with the display list
    /// resource storage that will play the filter.
    Image {
        frame_id: u64,
        src_rect: IntRect,
        dest_rect: IntRect,
        scaling_mode: ScalingMode,
    },
    Merge {
        inputs: Vec<Option<Filter>>,
    },
    Offset {
        dx: f32,
        dy: f32,
        input: Option<Box<Filter>>,
    },
    Erode {
        radius_x: f32,
        radius_y: f32,
        input: Option<Box<Filter>>,
    },
    Dilate {
        radius_x: f32,
        radius_y: f32,
        input: Option<Box<Filter>>,
    },
    Turbulence {
        kind: TurbulenceType,
        base_frequency_x: f32,
        base_frequency_y: f32,
        num_octaves: i32,
        seed: f32,
        tile_stitch_size: IntSize,
    },
    ColorSpaceConversion {
        source_color_space: InterpolationColorSpace,
        destination_color_space: InterpolationColorSpace,
        input: Option<Box<Filter>>,
    },
}

impl Filter {
    /// Applies `outer` to the output of `inner`.
    pub fn compose(outer: Filter, inner: Filter) -> Self {
        Self::Compose {
            outer: Box::new(outer),
            inner: Box::new(inner),
        }
    }

    /// The blur() filter function, which unlike [`Self::gaussian_blur`] mirrors at the edge of a
    /// backdrop.
    pub fn blur(radius_x: f32, radius_y: f32, input: Option<Filter>) -> Self {
        Self::Blur {
            radius_x,
            radius_y,
            mirrors_at_backdrop_edge: true,
            input: input.map(Box::new),
        }
    }

    /// A blur that reads its input as it is, such as an feGaussianBlur primitive.
    pub fn gaussian_blur(radius_x: f32, radius_y: f32, input: Option<Filter>) -> Self {
        Self::Blur {
            radius_x,
            radius_y,
            mirrors_at_backdrop_edge: false,
            input: input.map(Box::new),
        }
    }

    pub fn drop_shadow(offset_x: f32, offset_y: f32, radius: f32, color: Color, input: Option<Filter>) -> Self {
        Self::DropShadow {
            offset_x,
            offset_y,
            radius,
            color,
            input: input.map(Box::new),
        }
    }

    pub fn color(kind: ColorFilterType, amount: f32, input: Option<Filter>) -> Self {
        Self::ColorFilter {
            kind,
            amount,
            input: input.map(Box::new),
        }
    }

    pub fn hue_rotate(angle_degrees: f32, input: Option<Filter>) -> Self {
        Self::HueRotate {
            angle_degrees,
            input: input.map(Box::new),
        }
    }

    pub fn operation_type(&self) -> FilterOperationType {
        match self {
            Self::Arithmetic { .. } => FilterOperationType::Arithmetic,
            Self::Compose { .. } => FilterOperationType::Compose,
            Self::Blend { .. } => FilterOperationType::Blend,
            Self::Flood { .. } => FilterOperationType::Flood,
            Self::DisplacementMap { .. } => FilterOperationType::DisplacementMap,
            Self::DropShadow { .. } => FilterOperationType::DropShadow,
            Self::Blur { .. } => FilterOperationType::Blur,
            Self::ColorFilter { .. } => FilterOperationType::ColorFilter,
            Self::ColorMatrix { .. } => FilterOperationType::ColorMatrix,
            Self::ColorTable { .. } => FilterOperationType::ColorTable,
            Self::Saturate { .. } => FilterOperationType::Saturate,
            Self::HueRotate { .. } => FilterOperationType::HueRotate,
            Self::Image { .. } => FilterOperationType::Image,
            Self::Merge { .. } => FilterOperationType::Merge,
            Self::Offset { .. } => FilterOperationType::Offset,
            Self::Erode { .. } => FilterOperationType::Erode,
            Self::Dilate { .. } => FilterOperationType::Dilate,
            Self::Turbulence { .. } => FilterOperationType::Turbulence,
            Self::ColorSpaceConversion { .. } => FilterOperationType::ColorSpaceConversion,
        }
    }

    /// Visits every filter this one takes as an input, in serialization order.
    fn for_each_input(&self, visit: &mut impl FnMut(&Filter)) {
        let mut visit_optional = |input: Option<&Filter>| {
            if let Some(input) = input {
                visit(input);
            }
        };
        match self {
            Self::Arithmetic {
                background, foreground, ..
            }
            | Self::Blend {
                background, foreground, ..
            } => {
                visit_optional(background.as_deref());
                visit_optional(foreground.as_deref());
            }
            Self::Compose { outer, inner } => {
                visit(outer);
                visit(inner);
            }
            Self::DisplacementMap {
                color, displacement, ..
            } => {
                visit_optional(color.as_deref());
                visit_optional(displacement.as_deref());
            }
            Self::DropShadow { input, .. }
            | Self::Blur { input, .. }
            | Self::ColorFilter { input, .. }
            | Self::ColorMatrix { input, .. }
            | Self::ColorTable { input, .. }
            | Self::Saturate { input, .. }
            | Self::HueRotate { input, .. }
            | Self::Offset { input, .. }
            | Self::Erode { input, .. }
            | Self::Dilate { input, .. }
            | Self::ColorSpaceConversion { input, .. } => visit_optional(input.as_deref()),
            Self::Merge { inputs } => {
                for input in inputs.iter().flatten() {
                    visit(input);
                }
            }
            Self::Flood { .. } | Self::Image { .. } | Self::Turbulence { .. } => {}
        }
    }

    /// Visits the id of every image frame the graph draws, in serialization order.
    pub fn for_each_image_frame_id(&self, visit: &mut impl FnMut(u64)) {
        if let Self::Image { frame_id, .. } = self {
            visit(*frame_id);
        }
        self.for_each_input(&mut |input| input.for_each_image_frame_id(visit));
    }

    /// Whether the graph can paint outside the bounds its source graphic covers, or leave part
    /// of them unpainted. Operations that only remap the color channels of their input keep the
    /// alpha channel and therefore the painted bounds of that input; everything else is assumed
    /// to change them.
    pub fn may_affect_output_bounds(&self) -> bool {
        match self {
            Self::Compose { outer, inner } => outer.may_affect_output_bounds() || inner.may_affect_output_bounds(),
            Self::ColorFilter { input, .. }
            | Self::Saturate { input, .. }
            | Self::HueRotate { input, .. }
            | Self::ColorSpaceConversion { input, .. } => {
                input.as_ref().is_some_and(|input| input.may_affect_output_bounds())
            }
            _ => true,
        }
    }

    /// Whether the graph holds a blur that mirrors at the edge of a backdrop.
    fn has_mirroring_blur(&self) -> bool {
        if matches!(
            self,
            Self::Blur {
                mirrors_at_backdrop_edge: true,
                ..
            }
        ) {
            return true;
        }
        let mut found = false;
        self.for_each_input(&mut |input| found = found || input.has_mirroring_blur());
        found
    }

    /// [`Self::has_mirroring_blur`] for a serialized graph. Bytes that do not hold a graph have none.
    pub fn serialized_has_mirroring_blur(bytes: &[u8]) -> bool {
        Self::deserialize(bytes).is_some_and(|filter| filter.has_mirroring_blur())
    }

    /// [`Self::may_affect_output_bounds`] for a serialized graph, answered without building it.
    /// Bytes that do not hold a graph are assumed to affect the bounds.
    pub fn serialized_may_affect_output_bounds(bytes: &[u8]) -> bool {
        // Only the operations that keep their input's bounds have to be read; any other operation
        // settles the question by itself, so the walk stops there.
        fn node(decoder: &mut Decoder<'_>) -> Result<bool, DecodeError> {
            Ok(match decoder.decode()? {
                FilterOperationType::Compose => node(decoder)? || node(decoder)?,
                FilterOperationType::ColorFilter => {
                    decoder.decode::<ColorFilterType>()?;
                    decoder.decode::<f32>()?;
                    optional_input(decoder)?
                }
                FilterOperationType::Saturate | FilterOperationType::HueRotate => {
                    decoder.decode::<f32>()?;
                    optional_input(decoder)?
                }
                FilterOperationType::ColorSpaceConversion => {
                    decoder.decode::<InterpolationColorSpace>()?;
                    decoder.decode::<InterpolationColorSpace>()?;
                    optional_input(decoder)?
                }
                _ => true,
            })
        }
        fn optional_input(decoder: &mut Decoder<'_>) -> Result<bool, DecodeError> {
            if decoder.decode()? { node(decoder) } else { Ok(false) }
        }
        let mut decoder = Decoder::new(bytes, []);
        !matches!(node(&mut decoder), Ok(false)) || !decoder.is_at_end()
    }

    /// The bounds the graph paints within when its source graphic paints within `source`, in the
    /// same space, or `None` when the graph can paint everywhere.
    pub fn output_bounds(&self, source: FloatRect) -> Option<FloatRect> {
        self.bounds_for_source(OutputBounds::Rect(source), BoundsQuestion::Output)
            .into()
    }

    /// The bounds of the output that can change when the source graphic changes only within
    /// `changed_source`, together with everything the graph paints whatever its source, so that
    /// the graph appearing or disappearing is covered too. `None` means everywhere.
    ///
    /// Unlike the output bounds, these hold for a change in parts: the bounds of a change are the
    /// union of the bounds of its parts. An operation that keeps only where two of its inputs
    /// overlap paints within the overlap, but a change to either input can show anywhere the
    /// other one paints.
    pub fn changed_output_bounds(&self, changed_source: FloatRect) -> Option<FloatRect> {
        self.bounds_for_source(OutputBounds::Rect(changed_source), BoundsQuestion::Change)
            .into()
    }

    /// [`Self::output_bounds`] for a serialized graph. Bytes that do not hold a graph are assumed
    /// to paint everywhere.
    pub fn serialized_output_bounds(bytes: &[u8], source: FloatRect) -> Option<FloatRect> {
        Self::deserialize(bytes)?.output_bounds(source)
    }

    /// [`Self::changed_output_bounds`] for a serialized graph. Bytes that do not hold a graph are
    /// assumed to change everywhere.
    pub fn serialized_changed_output_bounds(bytes: &[u8], changed_source: FloatRect) -> Option<FloatRect> {
        Self::deserialize(bytes)?.changed_output_bounds(changed_source)
    }

    fn bounds_for_source(&self, source: OutputBounds, question: BoundsQuestion) -> OutputBounds {
        let input_bounds = |input: &Option<Box<Filter>>| {
            input
                .as_deref()
                .map_or(source, |input| input.bounds_for_source(source, question))
        };
        let painted_inputs = |painted_inputs: PaintedInputs| match (question, painted_inputs) {
            (BoundsQuestion::Change, PaintedInputs::Neither) | (BoundsQuestion::Output, _) => painted_inputs,
            (BoundsQuestion::Change, _) => PaintedInputs::Either,
        };
        match self {
            Self::Compose { outer, inner } => {
                outer.bounds_for_source(inner.bounds_for_source(source, question), question)
            }
            Self::ColorFilter { input, .. }
            | Self::Saturate { input, .. }
            | Self::HueRotate { input, .. }
            | Self::ColorSpaceConversion { input, .. } => input_bounds(input),
            // Erosion paints within its input, but a change spreads as far as dilation spreads it.
            Self::Erode {
                radius_x,
                radius_y,
                input,
            } => match question {
                BoundsQuestion::Output => input_bounds(input),
                BoundsQuestion::Change => {
                    input_bounds(input).outset(radius_x.max(0.0).ceil(), radius_y.max(0.0).ceil())
                }
            },
            // Only the alpha offset can turn transparent black into something visible.
            Self::ColorMatrix { matrix, input } => {
                if matrix[19] > 0.0 {
                    OutputBounds::Unbounded
                } else {
                    input_bounds(input)
                }
            }
            Self::ColorTable { a, input, .. } => {
                if a.as_ref().is_some_and(|alpha_table| alpha_table[0] != 0) {
                    OutputBounds::Unbounded
                } else {
                    input_bounds(input)
                }
            }
            Self::Blur {
                radius_x,
                radius_y,
                input,
                ..
            } => input_bounds(input).outset(gaussian_reach(*radius_x), gaussian_reach(*radius_y)),
            Self::DropShadow {
                offset_x,
                offset_y,
                radius,
                input,
                ..
            } => {
                let shape = input_bounds(input);
                let reach = gaussian_reach(*radius);
                shape.united(shape.outset(reach, reach).translated(*offset_x, *offset_y))
            }
            Self::Offset { dx, dy, input } => input_bounds(input).translated(*dx, *dy),
            Self::Dilate {
                radius_x,
                radius_y,
                input,
            } => input_bounds(input).outset(radius_x.max(0.0).ceil(), radius_y.max(0.0).ceil()),
            Self::DisplacementMap {
                color,
                displacement,
                scale,
                ..
            } => {
                let reach = (scale.abs() / 2.0).ceil();
                let displaced_color = input_bounds(color).outset(reach, reach);
                match question {
                    BoundsQuestion::Output => displaced_color,
                    // A change to the displacement moves what is sampled where it changed.
                    BoundsQuestion::Change => displaced_color.united(input_bounds(displacement)),
                }
            }
            Self::Merge { inputs } => inputs.iter().fold(OutputBounds::EMPTY, |bounds, input| {
                bounds.united(
                    input
                        .as_ref()
                        .map_or(source, |input| input.bounds_for_source(source, question)),
                )
            }),
            Self::Blend {
                background,
                foreground,
                mode,
            } => blended_output_bounds(
                painted_inputs(PaintedInputs::of_blend_mode(*mode)),
                input_bounds(foreground),
                input_bounds(background),
            ),
            Self::Arithmetic {
                background,
                foreground,
                k1,
                k2,
                k3,
                k4,
            } => {
                if *k4 != 0.0 {
                    return OutputBounds::Unbounded;
                }
                blended_output_bounds(
                    painted_inputs(PaintedInputs::of_arithmetic(*k1, *k2, *k3)),
                    input_bounds(foreground),
                    input_bounds(background),
                )
            }
            Self::Image { dest_rect, .. } => OutputBounds::Rect(dest_rect.to_float()),
            Self::Flood { .. } | Self::Turbulence { .. } => OutputBounds::Unbounded,
        }
    }

    pub fn serialize(&self) -> Vec<u8> {
        let mut encoder = Encoder::new();
        encoder.encode(self).expect("a filter encodes");
        encoder.finish().0
    }

    /// Reads a filter back from its serialized form. Fails on any malformed input, including bytes
    /// left over after the graph.
    pub fn deserialize(bytes: &[u8]) -> Option<Self> {
        let mut decoder = Decoder::new(bytes, []);
        let filter = decoder.decode().ok()?;
        decoder.is_at_end().then_some(filter)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
enum OutputBounds {
    Rect(FloatRect),
    Unbounded,
}

impl From<OutputBounds> for Option<FloatRect> {
    fn from(bounds: OutputBounds) -> Self {
        match bounds {
            OutputBounds::Rect(rect) => Some(rect),
            OutputBounds::Unbounded => None,
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum BoundsQuestion {
    // Where the output paints, for a source painting within the rect.
    Output,
    // Where the output can change, for a source changing within the rect.
    Change,
}

impl OutputBounds {
    const EMPTY: Self = Self::Rect(FloatRect::new(0.0, 0.0, 0.0, 0.0));

    fn united(self, other: Self) -> Self {
        match (self, other) {
            (Self::Rect(rect), Self::Rect(other)) => Self::Rect(rect.united(other)),
            _ => Self::Unbounded,
        }
    }

    fn intersected(self, other: Self) -> Self {
        match (self, other) {
            (Self::Rect(rect), Self::Rect(other)) => {
                let left = rect.x.max(other.x);
                let top = rect.y.max(other.y);
                let right = rect.right().min(other.right());
                let bottom = rect.bottom().min(other.bottom());
                if left >= right || top >= bottom {
                    return Self::EMPTY;
                }
                Self::Rect(FloatRect::new(left, top, right - left, bottom - top))
            }
            (Self::Unbounded, bounds) | (bounds, Self::Unbounded) => bounds,
        }
    }

    fn outset(self, x: f32, y: f32) -> Self {
        match self {
            Self::Rect(rect) if !rect.is_empty() => Self::Rect(FloatRect::new(
                rect.x - x,
                rect.y - y,
                rect.width + 2.0 * x,
                rect.height + 2.0 * y,
            )),
            bounds => bounds,
        }
    }

    fn translated(self, dx: f32, dy: f32) -> Self {
        match self {
            Self::Rect(rect) => Self::Rect(rect.translated(dx, dy)),
            Self::Unbounded => Self::Unbounded,
        }
    }
}

/// How far a Gaussian with this standard deviation spreads its input, matching the kernel extent
/// the Skia player uses.
fn gaussian_reach(sigma: f32) -> f32 {
    (3.0 * sigma.max(0.0)).ceil()
}

/// Which inputs a two-input operation can paint outside of transparent black over.
#[derive(Clone, Copy)]
enum PaintedInputs {
    Neither,
    Both,
    ForegroundOnly,
    BackgroundOnly,
    Either,
}

impl PaintedInputs {
    fn of_blend_mode(mode: CompositingAndBlendingOperator) -> Self {
        use CompositingAndBlendingOperator as Operator;
        match mode {
            Operator::Clear => Self::Neither,
            Operator::SourceIn | Operator::DestinationIn => Self::Both,
            Operator::Copy | Operator::SourceOut | Operator::DestinationATop => Self::ForegroundOnly,
            Operator::DestinationOut | Operator::SourceATop => Self::BackgroundOnly,
            _ => Self::Either,
        }
    }

    /// For `k1 * foreground * background + k2 * foreground + k3 * background`.
    fn of_arithmetic(k1: f32, k2: f32, k3: f32) -> Self {
        match (k2 != 0.0, k3 != 0.0) {
            (false, false) if k1 != 0.0 => Self::Both,
            (false, false) => Self::Neither,
            (true, false) => Self::ForegroundOnly,
            (false, true) => Self::BackgroundOnly,
            (true, true) => Self::Either,
        }
    }
}

fn blended_output_bounds(painted: PaintedInputs, foreground: OutputBounds, background: OutputBounds) -> OutputBounds {
    match painted {
        PaintedInputs::Neither => OutputBounds::EMPTY,
        PaintedInputs::Both => foreground.intersected(background),
        PaintedInputs::ForegroundOnly => foreground,
        PaintedInputs::BackgroundOnly => background,
        PaintedInputs::Either => foreground.united(background),
    }
}

impl Encode for FilterOperationType {
    #[inline]
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&(*self as u8))
    }
}

impl Decode for FilterOperationType {
    const MIN_ENCODED_SIZE: usize = size_of::<u8>();

    #[inline]
    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Self::from_u8(decoder.decode()?).ok_or(DecodeError::InvalidValue("Gfx::FilterOperationType"))
    }
}

impl Encode for Filter {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.operation_type())?;
        match self {
            Filter::Arithmetic {
                background,
                foreground,
                k1,
                k2,
                k3,
                k4,
            } => {
                encoder.encode(background)?;
                encoder.encode(foreground)?;
                encoder.encode(k1)?;
                encoder.encode(k2)?;
                encoder.encode(k3)?;
                encoder.encode(k4)
            }
            Filter::Compose { outer, inner } => {
                encoder.encode(outer)?;
                encoder.encode(inner)
            }
            Filter::Blend {
                background,
                foreground,
                mode,
            } => {
                encoder.encode(background)?;
                encoder.encode(foreground)?;
                encoder.encode(mode)
            }
            Filter::Flood { color, opacity } => {
                encoder.encode(color)?;
                encoder.encode(opacity)
            }
            Filter::DisplacementMap {
                color,
                displacement,
                scale,
                x_channel_selector,
                y_channel_selector,
            } => {
                encoder.encode(color)?;
                encoder.encode(displacement)?;
                encoder.encode(scale)?;
                encoder.encode(x_channel_selector)?;
                encoder.encode(y_channel_selector)
            }
            Filter::DropShadow {
                offset_x,
                offset_y,
                radius,
                color,
                input,
            } => {
                encoder.encode(offset_x)?;
                encoder.encode(offset_y)?;
                encoder.encode(radius)?;
                encoder.encode(color)?;
                encoder.encode(input)
            }
            Filter::Blur {
                radius_x,
                radius_y,
                mirrors_at_backdrop_edge,
                input,
            } => {
                encoder.encode(radius_x)?;
                encoder.encode(radius_y)?;
                encoder.encode(mirrors_at_backdrop_edge)?;
                encoder.encode(input)
            }
            Filter::Erode {
                radius_x,
                radius_y,
                input,
            }
            | Filter::Dilate {
                radius_x,
                radius_y,
                input,
            } => {
                encoder.encode(radius_x)?;
                encoder.encode(radius_y)?;
                encoder.encode(input)
            }
            Filter::ColorFilter { kind, amount, input } => {
                encoder.encode(kind)?;
                encoder.encode(amount)?;
                encoder.encode(input)
            }
            Filter::ColorMatrix { matrix, input } => {
                for value in matrix {
                    encoder.encode(value)?;
                }
                encoder.encode(input)
            }
            Filter::ColorTable { a, r, g, b, input } => {
                encoder.encode(a)?;
                encoder.encode(r)?;
                encoder.encode(g)?;
                encoder.encode(b)?;
                encoder.encode(input)
            }
            Filter::Saturate { value, input } => {
                encoder.encode(value)?;
                encoder.encode(input)
            }
            Filter::HueRotate { angle_degrees, input } => {
                encoder.encode(angle_degrees)?;
                encoder.encode(input)
            }
            Filter::Image {
                frame_id,
                src_rect,
                dest_rect,
                scaling_mode,
            } => {
                encoder.encode(frame_id)?;
                encoder.encode(src_rect)?;
                encoder.encode(dest_rect)?;
                encoder.encode(scaling_mode)
            }
            Filter::Merge { inputs } => encoder.encode(inputs),
            Filter::Offset { dx, dy, input } => {
                encoder.encode(dx)?;
                encoder.encode(dy)?;
                encoder.encode(input)
            }
            Filter::Turbulence {
                kind,
                base_frequency_x,
                base_frequency_y,
                num_octaves,
                seed,
                tile_stitch_size,
            } => {
                encoder.encode(kind)?;
                encoder.encode(base_frequency_x)?;
                encoder.encode(base_frequency_y)?;
                encoder.encode(num_octaves)?;
                encoder.encode(seed)?;
                encoder.encode(tile_stitch_size)
            }
            Filter::ColorSpaceConversion {
                source_color_space,
                destination_color_space,
                input,
            } => {
                encoder.encode(source_color_space)?;
                encoder.encode(destination_color_space)?;
                encoder.encode(input)
            }
        }
    }
}

impl Decode for Filter {
    const MIN_ENCODED_SIZE: usize = FilterOperationType::MIN_ENCODED_SIZE;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(match decoder.decode()? {
            FilterOperationType::Arithmetic => Filter::Arithmetic {
                background: decoder.decode()?,
                foreground: decoder.decode()?,
                k1: decoder.decode()?,
                k2: decoder.decode()?,
                k3: decoder.decode()?,
                k4: decoder.decode()?,
            },
            FilterOperationType::Compose => Filter::Compose {
                outer: decoder.decode()?,
                inner: decoder.decode()?,
            },
            FilterOperationType::Blend => Filter::Blend {
                background: decoder.decode()?,
                foreground: decoder.decode()?,
                mode: decoder.decode()?,
            },
            FilterOperationType::Flood => Filter::Flood {
                color: decoder.decode()?,
                opacity: decoder.decode()?,
            },
            FilterOperationType::DisplacementMap => Filter::DisplacementMap {
                color: decoder.decode()?,
                displacement: decoder.decode()?,
                scale: decoder.decode()?,
                x_channel_selector: decoder.decode()?,
                y_channel_selector: decoder.decode()?,
            },
            FilterOperationType::DropShadow => Filter::DropShadow {
                offset_x: decoder.decode()?,
                offset_y: decoder.decode()?,
                radius: decoder.decode()?,
                color: decoder.decode()?,
                input: decoder.decode()?,
            },
            FilterOperationType::Blur => Filter::Blur {
                radius_x: decoder.decode()?,
                radius_y: decoder.decode()?,
                mirrors_at_backdrop_edge: decoder.decode()?,
                input: decoder.decode()?,
            },
            FilterOperationType::ColorFilter => Filter::ColorFilter {
                kind: decoder.decode()?,
                amount: decoder.decode()?,
                input: decoder.decode()?,
            },
            FilterOperationType::ColorMatrix => {
                let mut matrix = [0.0; 20];
                for value in &mut matrix {
                    *value = decoder.decode()?;
                }
                Filter::ColorMatrix {
                    matrix,
                    input: decoder.decode()?,
                }
            }
            FilterOperationType::ColorTable => Filter::ColorTable {
                a: decoder.decode()?,
                r: decoder.decode()?,
                g: decoder.decode()?,
                b: decoder.decode()?,
                input: decoder.decode()?,
            },
            FilterOperationType::Saturate => Filter::Saturate {
                value: decoder.decode()?,
                input: decoder.decode()?,
            },
            FilterOperationType::HueRotate => Filter::HueRotate {
                angle_degrees: decoder.decode()?,
                input: decoder.decode()?,
            },
            FilterOperationType::Image => Filter::Image {
                frame_id: decoder.decode()?,
                src_rect: decoder.decode()?,
                dest_rect: decoder.decode()?,
                scaling_mode: decoder.decode()?,
            },
            FilterOperationType::Merge => Filter::Merge {
                inputs: decoder.decode()?,
            },
            FilterOperationType::Offset => Filter::Offset {
                dx: decoder.decode()?,
                dy: decoder.decode()?,
                input: decoder.decode()?,
            },
            FilterOperationType::Erode => Filter::Erode {
                radius_x: decoder.decode()?,
                radius_y: decoder.decode()?,
                input: decoder.decode()?,
            },
            FilterOperationType::Dilate => Filter::Dilate {
                radius_x: decoder.decode()?,
                radius_y: decoder.decode()?,
                input: decoder.decode()?,
            },
            FilterOperationType::Turbulence => Filter::Turbulence {
                kind: decoder.decode()?,
                base_frequency_x: decoder.decode()?,
                base_frequency_y: decoder.decode()?,
                num_octaves: decoder.decode()?,
                seed: decoder.decode()?,
                tile_stitch_size: decoder.decode()?,
            },
            FilterOperationType::ColorSpaceConversion => Filter::ColorSpaceConversion {
                source_color_space: decoder.decode()?,
                destination_color_space: decoder.decode()?,
                input: decoder.decode()?,
            },
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // The serialized form of every_operation_filter(). The Skia filter reader of the compositor
    // decodes the same layout, so a change that breaks this test needs the same change there.
    const EVERY_OPERATION_FILTER_BYTES: &str = concat!(
        "01020103332211ff0000003f010000010e0000c03f000020c0010c2a0000000000000001000000020000000300000004",
        "00000005000000060000000700000008000000010000000000803e0000003f0000403f0000803f130000000d04000000",
        "01050000803f000000400000404000ff007f0106000080400000a040010107020000000000003f00000104011100000000",
        "cdcccc3dcdcc4c3e030000000000e0401000000008000000010f0000803f0000803f0110000000400000004000000020",
        "41000000000300000001090100010000fffefdfcfbfaf9f8f7f6f5f4f3f2f1f0efeeedecebeae9e8e7e6e5e4e3e2e1e0",
        "dfdedddcdbdad9d8d7d6d5d4d3d2d1d0cfcecdcccbcac9c8c7c6c5c4c3c2c1c0bfbebdbcbbbab9b8b7b6b5b4b3b2b1b0",
        "afaeadacabaaa9a8a7a6a5a4a3a2a1a09f9e9d9c9b9a999897969594939291908f8e8d8c8b8a89888786858483828180",
        "7f7e7d7c7b7a797877767574737271706f6e6d6c6b6a696867666564636261605f5e5d5c5b5a59585756555453525150",
        "4f4e4d4c4b4a494847464544434241403f3e3d3c3b3a393837363534333231302f2e2d2c2b2a29282726252423222120",
        "1f1e1d1c1b1a191817161514131211100f0e0d0c0b0a090807060504030201000000000108000000000000003f000080",
        "3f0000c03f0000004000002040000040400000604000008040000090400000a0400000b0400000c0400000d0400000e0",
        "400000f04000000041000008410000104100001841010a9a99993e010b0000b4420112010000000000000000",
    );

    const IMAGE_FRAME_ID: u64 = 42;

    fn hex(bytes: &[u8]) -> String {
        bytes.iter().map(|byte| format!("{byte:02x}")).collect()
    }

    fn boxed(filter: Filter) -> Option<Box<Filter>> {
        Some(Box::new(filter))
    }

    // Every filter operation once, nested so that each input position is exercised.
    fn every_operation_filter() -> Filter {
        let image = Filter::Image {
            frame_id: IMAGE_FRAME_ID,
            src_rect: IntRect {
                x: 1,
                y: 2,
                width: 3,
                height: 4,
            },
            dest_rect: IntRect {
                x: 5,
                y: 6,
                width: 7,
                height: 8,
            },
            scaling_mode: ScalingMode::Bilinear,
        };
        let offset = Filter::Offset {
            dx: 1.5,
            dy: -2.5,
            input: boxed(image),
        };
        let arithmetic = Filter::Arithmetic {
            background: None,
            foreground: boxed(offset),
            k1: 0.25,
            k2: 0.5,
            k3: 0.75,
            k4: 1.0,
        };
        let flood = Filter::Flood {
            color: Color::from_rgba(0x11, 0x22, 0x33, 0xff),
            opacity: 0.5,
        };
        let blend = Filter::Blend {
            background: boxed(flood),
            foreground: boxed(arithmetic),
            mode: CompositingAndBlendingOperator::SourceOver,
        };

        let color = Filter::color(ColorFilterType::Grayscale, 0.5, None);
        let blur = Filter::blur(4.0, 5.0, Some(color));
        let drop_shadow = Filter::drop_shadow(1.0, 2.0, 3.0, Color::from_rgba(0x00, 0xff, 0x00, 0x7f), Some(blur));

        let turbulence = Filter::Turbulence {
            kind: TurbulenceType::FractalNoise,
            base_frequency_x: 0.1,
            base_frequency_y: 0.2,
            num_octaves: 3,
            seed: 7.0,
            tile_stitch_size: IntSize { width: 16, height: 8 },
        };
        let dilate = Filter::Dilate {
            radius_x: 2.0,
            radius_y: 2.0,
            input: None,
        };
        let erode = Filter::Erode {
            radius_x: 1.0,
            radius_y: 1.0,
            input: boxed(dilate),
        };
        let displacement_map = Filter::DisplacementMap {
            color: boxed(turbulence),
            displacement: boxed(erode),
            scale: 10.0,
            x_channel_selector: ChannelSelector::Red,
            y_channel_selector: ChannelSelector::Alpha,
        };

        let color_space_conversion = Filter::ColorSpaceConversion {
            source_color_space: InterpolationColorSpace::SRGB,
            destination_color_space: InterpolationColorSpace::LinearRGB,
            input: None,
        };
        let hue_rotate = Filter::hue_rotate(90.0, Some(color_space_conversion));
        let saturate = Filter::Saturate {
            value: 0.3,
            input: boxed(hue_rotate),
        };
        let color_matrix = Filter::ColorMatrix {
            matrix: std::array::from_fn(|index| index as f32 * 0.5),
            input: boxed(saturate),
        };
        let color_table = Filter::ColorTable {
            a: Some(Box::new(std::array::from_fn(|index| 255 - index as u8))),
            r: None,
            g: None,
            b: None,
            input: boxed(color_matrix),
        };

        let merge = Filter::Merge {
            inputs: vec![Some(drop_shadow), None, Some(displacement_map), Some(color_table)],
        };
        Filter::compose(blend, merge)
    }

    #[test]
    #[cfg(target_endian = "little")]
    fn every_operation_serializes_to_the_shared_bytes() {
        assert_eq!(hex(&every_operation_filter().serialize()), EVERY_OPERATION_FILTER_BYTES);
    }

    #[test]
    fn every_operation_round_trips() {
        let filter = every_operation_filter();
        assert_eq!(Filter::deserialize(&filter.serialize()), Some(filter));
    }

    #[test]
    fn deserialize_rejects_truncated_and_padded_input() {
        let bytes = every_operation_filter().serialize();
        for length in 0..bytes.len() {
            assert!(
                Filter::deserialize(&bytes[..length]).is_none(),
                "accepted {length} bytes"
            );
        }
        let mut padded = bytes.clone();
        padded.push(0);
        assert!(Filter::deserialize(&padded).is_none());
    }

    #[test]
    fn deserialize_rejects_unknown_values() {
        let mut bytes = Filter::blur(1.0, 1.0, None).serialize();
        bytes[0] = FilterOperationType::ColorSpaceConversion as u8 + 1;
        assert!(Filter::deserialize(&bytes).is_none());

        let mut bytes = Filter::color(ColorFilterType::Sepia, 1.0, None).serialize();
        bytes[1..5].copy_from_slice(&(ColorFilterType::Sepia as i32 + 1).to_ne_bytes());
        assert!(Filter::deserialize(&bytes).is_none());

        let table = Filter::ColorTable {
            a: Some(Box::new([0; 256])),
            r: None,
            g: None,
            b: None,
            input: None,
        };
        let mut bytes = table.serialize();
        bytes[2..6].copy_from_slice(&255u32.to_ne_bytes());
        assert!(Filter::deserialize(&bytes).is_none());
    }

    #[test]
    fn color_only_operations_keep_the_bounds_of_their_input() {
        let color = || Filter::color(ColorFilterType::Invert, 1.0, None);
        assert!(!color().may_affect_output_bounds());
        assert!(!Filter::hue_rotate(45.0, Some(color())).may_affect_output_bounds());
        assert!(!Filter::compose(color(), Filter::hue_rotate(45.0, None)).may_affect_output_bounds());
        assert!(Filter::blur(1.0, 1.0, None).may_affect_output_bounds());
        assert!(Filter::compose(color(), Filter::blur(1.0, 1.0, None)).may_affect_output_bounds());
        assert!(Filter::hue_rotate(45.0, Some(Filter::blur(1.0, 1.0, None))).may_affect_output_bounds());
        assert!(
            Filter::ColorMatrix {
                matrix: [0.0; 20],
                input: None
            }
            .may_affect_output_bounds()
        );
    }

    #[test]
    fn the_serialized_bounds_walk_agrees_with_the_graph() {
        let color = || Filter::color(ColorFilterType::Invert, 1.0, None);
        let graphs = [
            every_operation_filter(),
            color(),
            Filter::hue_rotate(45.0, Some(color())),
            Filter::compose(color(), Filter::hue_rotate(45.0, None)),
            Filter::compose(color(), Filter::blur(1.0, 1.0, None)),
            Filter::compose(Filter::blur(1.0, 1.0, None), color()),
            Filter::Saturate {
                value: 0.5,
                input: Some(Box::new(Filter::ColorSpaceConversion {
                    source_color_space: InterpolationColorSpace::SRGB,
                    destination_color_space: InterpolationColorSpace::LinearRGB,
                    input: None,
                })),
            },
        ];
        for graph in graphs {
            assert_eq!(
                Filter::serialized_may_affect_output_bounds(&graph.serialize()),
                graph.may_affect_output_bounds(),
                "{graph:?}"
            );
        }
        assert!(Filter::serialized_may_affect_output_bounds(&[]));
        assert!(Filter::serialized_may_affect_output_bounds(&[0xff]));
        let mut padded = color().serialize();
        padded.push(0);
        assert!(Filter::serialized_may_affect_output_bounds(&padded));
    }

    #[test]
    fn every_operation_reports_the_bounds_it_paints_within() {
        let source = FloatRect::new(10.0, 20.0, 30.0, 40.0);
        let flood = || Filter::Flood {
            color: Color::from_rgba(0, 0, 0, 0xff),
            opacity: 1.0,
        };
        let offset = |dx, dy| Filter::Offset { dx, dy, input: None };
        let blend = |mode| Filter::Blend {
            background: boxed(offset(100.0, 0.0)),
            foreground: None,
            mode,
        };
        let arithmetic = |k1, k2, k3, k4| Filter::Arithmetic {
            background: boxed(offset(15.0, 0.0)),
            foreground: None,
            k1,
            k2,
            k3,
            k4,
        };
        let mut alpha_table = Box::new([0u8; 256]);
        let cases: Vec<(Filter, Option<FloatRect>)> = vec![
            (Filter::color(ColorFilterType::Opacity, 0.5, None), Some(source)),
            (Filter::hue_rotate(45.0, None), Some(source)),
            (
                Filter::ColorMatrix {
                    matrix: [0.0; 20],
                    input: None,
                },
                Some(source),
            ),
            (
                Filter::ColorMatrix {
                    matrix: std::array::from_fn(|index| if index == 19 { 1.0 } else { 0.0 }),
                    input: None,
                },
                None,
            ),
            (
                Filter::ColorTable {
                    a: Some(alpha_table.clone()),
                    r: None,
                    g: None,
                    b: None,
                    input: None,
                },
                Some(source),
            ),
            (
                Filter::ColorTable {
                    a: Some({
                        alpha_table[0] = 1;
                        alpha_table
                    }),
                    r: None,
                    g: None,
                    b: None,
                    input: None,
                },
                None,
            ),
            (
                Filter::blur(2.0, 0.5, None),
                Some(FloatRect::new(4.0, 18.0, 42.0, 44.0)),
            ),
            (
                Filter::drop_shadow(5.0, 50.0, 1.0, Color::from_rgba(0, 0, 0, 0xff), None),
                Some(FloatRect::new(10.0, 20.0, 38.0, 93.0)),
            ),
            (offset(-5.0, 2.5), Some(FloatRect::new(5.0, 22.5, 30.0, 40.0))),
            (
                Filter::Dilate {
                    radius_x: 1.5,
                    radius_y: 0.0,
                    input: None,
                },
                Some(FloatRect::new(8.0, 20.0, 34.0, 40.0)),
            ),
            (
                Filter::Erode {
                    radius_x: 3.0,
                    radius_y: 3.0,
                    input: None,
                },
                Some(source),
            ),
            (
                Filter::DisplacementMap {
                    color: None,
                    displacement: boxed(flood()),
                    scale: -5.0,
                    x_channel_selector: ChannelSelector::Red,
                    y_channel_selector: ChannelSelector::Green,
                },
                Some(FloatRect::new(7.0, 17.0, 36.0, 46.0)),
            ),
            (
                Filter::Merge {
                    inputs: vec![None, Some(offset(0.0, 100.0))],
                },
                Some(FloatRect::new(10.0, 20.0, 30.0, 140.0)),
            ),
            (Filter::Merge { inputs: vec![] }, Some(FloatRect::default())),
            (blend(CompositingAndBlendingOperator::Clear), Some(FloatRect::default())),
            (
                blend(CompositingAndBlendingOperator::SourceIn),
                Some(FloatRect::default()),
            ),
            (blend(CompositingAndBlendingOperator::Copy), Some(source)),
            (
                blend(CompositingAndBlendingOperator::SourceATop),
                Some(source.translated(100.0, 0.0)),
            ),
            (
                blend(CompositingAndBlendingOperator::Multiply),
                Some(FloatRect::new(10.0, 20.0, 130.0, 40.0)),
            ),
            (
                blend(CompositingAndBlendingOperator::PlusLighter),
                Some(FloatRect::new(10.0, 20.0, 130.0, 40.0)),
            ),
            (
                arithmetic(1.0, 0.0, 0.0, 0.0),
                Some(FloatRect::new(25.0, 20.0, 15.0, 40.0)),
            ),
            (arithmetic(1.0, 1.0, 0.0, 0.0), Some(source)),
            (arithmetic(0.0, 0.0, 1.0, 0.0), Some(source.translated(15.0, 0.0))),
            (
                arithmetic(0.0, 1.0, 1.0, 0.0),
                Some(FloatRect::new(10.0, 20.0, 45.0, 40.0)),
            ),
            (arithmetic(0.0, 0.0, 0.0, 0.0), Some(FloatRect::default())),
            (arithmetic(0.0, 0.0, 0.0, 0.5), None),
            (
                Filter::Image {
                    frame_id: IMAGE_FRAME_ID,
                    src_rect: IntRect::default(),
                    dest_rect: IntRect {
                        x: 1,
                        y: 2,
                        width: 3,
                        height: 4,
                    },
                    scaling_mode: ScalingMode::None,
                },
                Some(FloatRect::new(1.0, 2.0, 3.0, 4.0)),
            ),
            (flood(), None),
            (
                Filter::Turbulence {
                    kind: TurbulenceType::Turbulence,
                    base_frequency_x: 0.1,
                    base_frequency_y: 0.1,
                    num_octaves: 1,
                    seed: 0.0,
                    tile_stitch_size: IntSize::default(),
                },
                None,
            ),
            (
                Filter::compose(offset(1.0, 0.0), Filter::blur(1.0, 1.0, None)),
                Some(FloatRect::new(8.0, 17.0, 36.0, 46.0)),
            ),
            // An unbounded flood masked by the blurred source alpha, as SVG lowers feDropShadow.
            (
                Filter::Blend {
                    background: boxed(Filter::blur(1.0, 1.0, None)),
                    foreground: boxed(flood()),
                    mode: CompositingAndBlendingOperator::SourceIn,
                },
                Some(FloatRect::new(7.0, 17.0, 36.0, 46.0)),
            ),
            (every_operation_filter(), None),
        ];
        for (graph, expected) in cases {
            assert_eq!(graph.output_bounds(source), expected, "{graph:?}");
        }
    }

    #[test]
    fn a_change_shows_wherever_any_input_carries_it() {
        let changed_source = FloatRect::new(50.0, 10.0, 20.0, 20.0);
        let offset = |dx| Filter::Offset {
            dx,
            dy: 0.0,
            input: None,
        };
        // The source masked by a copy of itself shifted right: a change shows where either copy is.
        let masked_by_shifted_copy = Filter::Blend {
            background: boxed(offset(40.0)),
            foreground: None,
            mode: CompositingAndBlendingOperator::SourceIn,
        };
        assert_eq!(
            masked_by_shifted_copy.output_bounds(changed_source),
            Some(FloatRect::default())
        );
        assert_eq!(
            masked_by_shifted_copy.changed_output_bounds(changed_source),
            Some(FloatRect::new(50.0, 10.0, 60.0, 20.0))
        );
        let overlap_only = Filter::Arithmetic {
            background: boxed(offset(40.0)),
            foreground: None,
            k1: 1.0,
            k2: 0.0,
            k3: 0.0,
            k4: 0.0,
        };
        assert_eq!(
            overlap_only.changed_output_bounds(changed_source),
            Some(FloatRect::new(50.0, 10.0, 60.0, 20.0))
        );
        assert_eq!(
            Filter::Erode {
                radius_x: 2.0,
                radius_y: 0.5,
                input: None,
            }
            .changed_output_bounds(changed_source),
            Some(FloatRect::new(48.0, 9.0, 24.0, 22.0))
        );
        assert_eq!(
            Filter::blur(1.0, 1.0, None).changed_output_bounds(changed_source),
            Filter::blur(1.0, 1.0, None).output_bounds(changed_source)
        );
        // The source shifted far away and displaced by the source itself: a change moves what is
        // sampled where the change is, not only around the shifted copy.
        let displaced_by_source = Filter::DisplacementMap {
            color: boxed(offset(100.0)),
            displacement: None,
            scale: 10.0,
            x_channel_selector: ChannelSelector::Red,
            y_channel_selector: ChannelSelector::Green,
        };
        assert_eq!(
            displaced_by_source.output_bounds(changed_source),
            Some(FloatRect::new(145.0, 5.0, 30.0, 30.0))
        );
        assert_eq!(
            displaced_by_source.changed_output_bounds(changed_source),
            Some(FloatRect::new(50.0, 5.0, 125.0, 30.0))
        );
        let cleared = Filter::Blend {
            background: None,
            foreground: None,
            mode: CompositingAndBlendingOperator::Clear,
        };
        assert_eq!(
            cleared.changed_output_bounds(changed_source),
            Some(FloatRect::default())
        );
        // What a graph paints whatever its source is part of the change too.
        let flood = Filter::Flood {
            color: Color::from_rgba(0, 0, 0, 0xff),
            opacity: 1.0,
        };
        assert_eq!(flood.changed_output_bounds(changed_source), None);
        let image = Filter::Image {
            frame_id: IMAGE_FRAME_ID,
            src_rect: IntRect::default(),
            dest_rect: IntRect {
                x: 1,
                y: 2,
                width: 3,
                height: 4,
            },
            scaling_mode: ScalingMode::None,
        };
        assert_eq!(
            Filter::Merge {
                inputs: vec![Some(image), None],
            }
            .changed_output_bounds(changed_source),
            Some(FloatRect::new(1.0, 2.0, 69.0, 28.0))
        );
        for graph in [
            masked_by_shifted_copy,
            overlap_only,
            displaced_by_source,
            every_operation_filter(),
        ] {
            assert_eq!(
                Filter::serialized_changed_output_bounds(&graph.serialize(), changed_source),
                graph.changed_output_bounds(changed_source),
                "{graph:?}"
            );
        }
        assert_eq!(Filter::serialized_changed_output_bounds(&[0xff], changed_source), None);
    }

    #[test]
    fn an_empty_source_stays_empty_through_spreading_operations() {
        let empty = FloatRect::default();
        assert_eq!(Filter::blur(5.0, 5.0, None).output_bounds(empty), Some(empty));
        assert_eq!(
            Filter::Dilate {
                radius_x: 5.0,
                radius_y: 5.0,
                input: None,
            }
            .output_bounds(empty),
            Some(empty)
        );
    }

    #[test]
    fn the_serialized_output_bounds_agree_with_the_graph() {
        let source = FloatRect::new(10.0, 20.0, 30.0, 40.0);
        let graphs = [
            every_operation_filter(),
            Filter::blur(2.0, 3.0, None),
            Filter::compose(
                Filter::drop_shadow(1.0, 2.0, 3.0, Color::from_rgba(0, 0, 0, 0xff), None),
                Filter::hue_rotate(45.0, None),
            ),
        ];
        for graph in graphs {
            assert_eq!(
                Filter::serialized_output_bounds(&graph.serialize(), source),
                graph.output_bounds(source),
                "{graph:?}"
            );
        }
        assert_eq!(Filter::serialized_output_bounds(&[], source), None);
        assert_eq!(Filter::serialized_output_bounds(&[0xff], source), None);
    }

    #[test]
    fn image_frame_ids_are_visited_in_serialization_order() {
        let image = |frame_id| Filter::Image {
            frame_id,
            src_rect: IntRect::default(),
            dest_rect: IntRect::default(),
            scaling_mode: ScalingMode::None,
        };
        let filter = Filter::Merge {
            inputs: vec![
                Some(image(1)),
                None,
                Some(Filter::compose(image(2), Filter::blur(1.0, 1.0, Some(image(3))))),
            ],
        };
        let mut ids = Vec::new();
        filter.for_each_image_frame_id(&mut |id| ids.push(id));
        assert_eq!(ids, [1, 2, 3]);
        let mut ids = Vec::new();
        Filter::deserialize(&filter.serialize())
            .unwrap()
            .for_each_image_frame_id(&mut |id| ids.push(id));
        assert_eq!(ids, [1, 2, 3]);
    }
}
