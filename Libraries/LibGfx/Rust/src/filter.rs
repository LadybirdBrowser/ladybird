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

use std::ffi::c_void;

use crate::{
    Color, ColorFilterType, CompositingAndBlendingOperator, IntRect, IntSize, InterpolationColorSpace, ScalingMode,
};

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

    pub fn blur(radius_x: f32, radius_y: f32, input: Option<Filter>) -> Self {
        Self::Blur {
            radius_x,
            radius_y,
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

    /// [`Self::may_affect_output_bounds`] for a serialized graph, answered without building it.
    /// Bytes that do not hold a graph are assumed to affect the bounds.
    pub fn serialized_may_affect_output_bounds(bytes: &[u8]) -> bool {
        // Only the operations that keep their input's bounds have to be read; any other operation
        // settles the question by itself, so the walk stops there.
        fn node(reader: &mut Reader) -> Option<bool> {
            Some(match FilterOperationType::from_u8(reader.u8()?)? {
                FilterOperationType::Compose => node(reader)? || node(reader)?,
                FilterOperationType::ColorFilter => {
                    reader.i32()?;
                    reader.f32()?;
                    optional_input(reader)?
                }
                FilterOperationType::Saturate | FilterOperationType::HueRotate => {
                    reader.f32()?;
                    optional_input(reader)?
                }
                FilterOperationType::ColorSpaceConversion => {
                    reader.i32()?;
                    reader.i32()?;
                    optional_input(reader)?
                }
                _ => true,
            })
        }
        fn optional_input(reader: &mut Reader) -> Option<bool> {
            if reader.bool()? { node(reader) } else { Some(false) }
        }
        let mut reader = Reader { bytes, offset: 0 };
        node(&mut reader).is_none_or(|affects_bounds| affects_bounds || reader.offset != bytes.len())
    }

    pub fn serialize(&self) -> Vec<u8> {
        let mut bytes = Vec::new();
        Writer { bytes: &mut bytes }.filter(self);
        bytes
    }

    /// Reads a filter back from its serialized form. Fails on any malformed input, including bytes
    /// left over after the graph.
    pub fn deserialize(bytes: &[u8]) -> Option<Self> {
        let mut reader = Reader { bytes, offset: 0 };
        let filter = reader.filter()?;
        (reader.offset == bytes.len()).then_some(filter)
    }
}

struct Writer<'a> {
    bytes: &'a mut Vec<u8>,
}

impl Writer<'_> {
    fn u8(&mut self, value: u8) {
        self.bytes.push(value);
    }

    fn bool(&mut self, value: bool) {
        self.u8(u8::from(value));
    }

    fn i32(&mut self, value: i32) {
        self.bytes.extend_from_slice(&value.to_ne_bytes());
    }

    fn u32(&mut self, value: u32) {
        self.bytes.extend_from_slice(&value.to_ne_bytes());
    }

    fn u64(&mut self, value: u64) {
        self.bytes.extend_from_slice(&value.to_ne_bytes());
    }

    fn f32(&mut self, value: f32) {
        self.bytes.extend_from_slice(&value.to_ne_bytes());
    }

    fn color(&mut self, color: Color) {
        self.u32(color.0);
    }

    fn int_rect(&mut self, rect: IntRect) {
        self.i32(rect.x);
        self.i32(rect.y);
        self.i32(rect.width);
        self.i32(rect.height);
    }

    fn int_size(&mut self, size: IntSize) {
        self.i32(size.width);
        self.i32(size.height);
    }

    fn channel_table(&mut self, table: Option<&ChannelTable>) {
        self.bool(table.is_some());
        if let Some(table) = table {
            self.u32(table.len() as u32);
            self.bytes.extend_from_slice(table.as_slice());
        }
    }

    fn optional_filter(&mut self, filter: Option<&Filter>) {
        self.bool(filter.is_some());
        if let Some(filter) = filter {
            self.filter(filter);
        }
    }

    fn filter(&mut self, filter: &Filter) {
        self.u8(filter.operation_type() as u8);
        match filter {
            Filter::Arithmetic {
                background,
                foreground,
                k1,
                k2,
                k3,
                k4,
            } => {
                self.optional_filter(background.as_deref());
                self.optional_filter(foreground.as_deref());
                self.f32(*k1);
                self.f32(*k2);
                self.f32(*k3);
                self.f32(*k4);
            }
            Filter::Compose { outer, inner } => {
                self.filter(outer);
                self.filter(inner);
            }
            Filter::Blend {
                background,
                foreground,
                mode,
            } => {
                self.optional_filter(background.as_deref());
                self.optional_filter(foreground.as_deref());
                self.i32(*mode as i32);
            }
            Filter::Flood { color, opacity } => {
                self.color(*color);
                self.f32(*opacity);
            }
            Filter::DisplacementMap {
                color,
                displacement,
                scale,
                x_channel_selector,
                y_channel_selector,
            } => {
                self.optional_filter(color.as_deref());
                self.optional_filter(displacement.as_deref());
                self.f32(*scale);
                self.i32(*x_channel_selector as i32);
                self.i32(*y_channel_selector as i32);
            }
            Filter::DropShadow {
                offset_x,
                offset_y,
                radius,
                color,
                input,
            } => {
                self.f32(*offset_x);
                self.f32(*offset_y);
                self.f32(*radius);
                self.color(*color);
                self.optional_filter(input.as_deref());
            }
            Filter::Blur {
                radius_x,
                radius_y,
                input,
            }
            | Filter::Erode {
                radius_x,
                radius_y,
                input,
            }
            | Filter::Dilate {
                radius_x,
                radius_y,
                input,
            } => {
                self.f32(*radius_x);
                self.f32(*radius_y);
                self.optional_filter(input.as_deref());
            }
            Filter::ColorFilter { kind, amount, input } => {
                self.i32(*kind as i32);
                self.f32(*amount);
                self.optional_filter(input.as_deref());
            }
            Filter::ColorMatrix { matrix, input } => {
                for value in matrix {
                    self.f32(*value);
                }
                self.optional_filter(input.as_deref());
            }
            Filter::ColorTable { a, r, g, b, input } => {
                self.channel_table(a.as_ref());
                self.channel_table(r.as_ref());
                self.channel_table(g.as_ref());
                self.channel_table(b.as_ref());
                self.optional_filter(input.as_deref());
            }
            Filter::Saturate { value, input } => {
                self.f32(*value);
                self.optional_filter(input.as_deref());
            }
            Filter::HueRotate { angle_degrees, input } => {
                self.f32(*angle_degrees);
                self.optional_filter(input.as_deref());
            }
            Filter::Image {
                frame_id,
                src_rect,
                dest_rect,
                scaling_mode,
            } => {
                self.u64(*frame_id);
                self.int_rect(*src_rect);
                self.int_rect(*dest_rect);
                self.i32(*scaling_mode as i32);
            }
            Filter::Merge { inputs } => {
                self.u32(inputs.len() as u32);
                for input in inputs {
                    self.optional_filter(input.as_ref());
                }
            }
            Filter::Offset { dx, dy, input } => {
                self.f32(*dx);
                self.f32(*dy);
                self.optional_filter(input.as_deref());
            }
            Filter::Turbulence {
                kind,
                base_frequency_x,
                base_frequency_y,
                num_octaves,
                seed,
                tile_stitch_size,
            } => {
                self.i32(*kind as i32);
                self.f32(*base_frequency_x);
                self.f32(*base_frequency_y);
                self.i32(*num_octaves);
                self.f32(*seed);
                self.int_size(*tile_stitch_size);
            }
            Filter::ColorSpaceConversion {
                source_color_space,
                destination_color_space,
                input,
            } => {
                self.i32(*source_color_space as i32);
                self.i32(*destination_color_space as i32);
                self.optional_filter(input.as_deref());
            }
        }
    }
}

struct Reader<'a> {
    bytes: &'a [u8],
    offset: usize,
}

impl Reader<'_> {
    fn take<const N: usize>(&mut self) -> Option<[u8; N]> {
        let end = self.offset.checked_add(N)?;
        let value = self.bytes.get(self.offset..end)?.try_into().ok()?;
        self.offset = end;
        Some(value)
    }

    fn u8(&mut self) -> Option<u8> {
        self.take::<1>().map(|[value]| value)
    }

    fn bool(&mut self) -> Option<bool> {
        self.u8().map(|value| value != 0)
    }

    fn i32(&mut self) -> Option<i32> {
        self.take().map(i32::from_ne_bytes)
    }

    fn u32(&mut self) -> Option<u32> {
        self.take().map(u32::from_ne_bytes)
    }

    fn u64(&mut self) -> Option<u64> {
        self.take().map(u64::from_ne_bytes)
    }

    fn f32(&mut self) -> Option<f32> {
        self.take().map(f32::from_ne_bytes)
    }

    fn color(&mut self) -> Option<Color> {
        self.u32().map(Color)
    }

    fn int_rect(&mut self) -> Option<IntRect> {
        Some(IntRect {
            x: self.i32()?,
            y: self.i32()?,
            width: self.i32()?,
            height: self.i32()?,
        })
    }

    fn int_size(&mut self) -> Option<IntSize> {
        Some(IntSize {
            width: self.i32()?,
            height: self.i32()?,
        })
    }

    fn channel_table(&mut self) -> Option<Option<ChannelTable>> {
        if !self.bool()? {
            return Some(None);
        }
        if self.u32()? != 256 {
            return None;
        }
        self.take::<256>().map(|table| Some(Box::new(table)))
    }

    fn optional_filter(&mut self) -> Option<Option<Filter>> {
        if !self.bool()? {
            return Some(None);
        }
        self.filter().map(Some)
    }

    fn optional_input(&mut self) -> Option<Option<Box<Filter>>> {
        self.optional_filter().map(|input| input.map(Box::new))
    }

    fn filter(&mut self) -> Option<Filter> {
        let filter = match FilterOperationType::from_u8(self.u8()?)? {
            FilterOperationType::Arithmetic => Filter::Arithmetic {
                background: self.optional_input()?,
                foreground: self.optional_input()?,
                k1: self.f32()?,
                k2: self.f32()?,
                k3: self.f32()?,
                k4: self.f32()?,
            },
            FilterOperationType::Compose => Filter::Compose {
                outer: Box::new(self.filter()?),
                inner: Box::new(self.filter()?),
            },
            FilterOperationType::Blend => Filter::Blend {
                background: self.optional_input()?,
                foreground: self.optional_input()?,
                mode: CompositingAndBlendingOperator::from_i32(self.i32()?)?,
            },
            FilterOperationType::Flood => Filter::Flood {
                color: self.color()?,
                opacity: self.f32()?,
            },
            FilterOperationType::DisplacementMap => Filter::DisplacementMap {
                color: self.optional_input()?,
                displacement: self.optional_input()?,
                scale: self.f32()?,
                x_channel_selector: ChannelSelector::from_i32(self.i32()?)?,
                y_channel_selector: ChannelSelector::from_i32(self.i32()?)?,
            },
            FilterOperationType::DropShadow => Filter::DropShadow {
                offset_x: self.f32()?,
                offset_y: self.f32()?,
                radius: self.f32()?,
                color: self.color()?,
                input: self.optional_input()?,
            },
            FilterOperationType::Blur => Filter::Blur {
                radius_x: self.f32()?,
                radius_y: self.f32()?,
                input: self.optional_input()?,
            },
            FilterOperationType::ColorFilter => Filter::ColorFilter {
                kind: ColorFilterType::from_i32(self.i32()?)?,
                amount: self.f32()?,
                input: self.optional_input()?,
            },
            FilterOperationType::ColorMatrix => {
                let mut matrix = [0.0; 20];
                for value in &mut matrix {
                    *value = self.f32()?;
                }
                Filter::ColorMatrix {
                    matrix,
                    input: self.optional_input()?,
                }
            }
            FilterOperationType::ColorTable => Filter::ColorTable {
                a: self.channel_table()?,
                r: self.channel_table()?,
                g: self.channel_table()?,
                b: self.channel_table()?,
                input: self.optional_input()?,
            },
            FilterOperationType::Saturate => Filter::Saturate {
                value: self.f32()?,
                input: self.optional_input()?,
            },
            FilterOperationType::HueRotate => Filter::HueRotate {
                angle_degrees: self.f32()?,
                input: self.optional_input()?,
            },
            FilterOperationType::Image => Filter::Image {
                frame_id: self.u64()?,
                src_rect: self.int_rect()?,
                dest_rect: self.int_rect()?,
                scaling_mode: ScalingMode::from_i32(self.i32()?)?,
            },
            FilterOperationType::Merge => {
                let count = self.u32()?;
                let mut inputs = Vec::new();
                for _ in 0..count {
                    inputs.push(self.optional_filter()?);
                }
                Filter::Merge { inputs }
            }
            FilterOperationType::Offset => Filter::Offset {
                dx: self.f32()?,
                dy: self.f32()?,
                input: self.optional_input()?,
            },
            FilterOperationType::Erode => Filter::Erode {
                radius_x: self.f32()?,
                radius_y: self.f32()?,
                input: self.optional_input()?,
            },
            FilterOperationType::Dilate => Filter::Dilate {
                radius_x: self.f32()?,
                radius_y: self.f32()?,
                input: self.optional_input()?,
            },
            FilterOperationType::Turbulence => Filter::Turbulence {
                kind: TurbulenceType::from_i32(self.i32()?)?,
                base_frequency_x: self.f32()?,
                base_frequency_y: self.f32()?,
                num_octaves: self.i32()?,
                seed: self.f32()?,
                tile_stitch_size: self.int_size()?,
            },
            FilterOperationType::ColorSpaceConversion => Filter::ColorSpaceConversion {
                source_color_space: InterpolationColorSpace::from_i32(self.i32()?)?,
                destination_color_space: InterpolationColorSpace::from_i32(self.i32()?)?,
                input: self.optional_input()?,
            },
        };
        Some(filter)
    }
}

/// Visits the id of every image frame a serialized filter draws, for a C++ caller that holds only
/// the bytes. Bytes that do not hold a filter visit nothing.
///
/// # Safety
///
/// `bytes` must point to `count` readable bytes, and `visit` must accept `context`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ladybird_gfx_filter_for_each_image_frame_id(
    bytes: *const u8,
    count: usize,
    visit: unsafe extern "C" fn(*const c_void, u64),
    context: *const c_void,
) {
    let bytes = if count == 0 {
        &[]
    } else {
        // SAFETY: The caller guarantees `count` readable bytes.
        unsafe { std::slice::from_raw_parts(bytes, count) }
    };
    if let Some(filter) = Filter::deserialize(bytes) {
        // SAFETY: The caller guarantees `visit` accepts `context`.
        filter.for_each_image_frame_id(&mut |id| unsafe { visit(context, id) });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // The serialized form of every_operation_filter(). Tests/LibGfx/TestFilter.cpp checks the same
    // bytes against the C++ codec, so the two sides cannot drift apart without a test noticing.
    const EVERY_OPERATION_FILTER_BYTES: &str = concat!(
        "01020103332211ff0000003f010000010e0000c03f000020c0010c2a0000000000000001000000020000000300000004",
        "00000005000000060000000700000008000000010000000000803e0000003f0000403f0000803f130000000d04000000",
        "01050000803f000000400000404000ff007f0106000080400000a0400107020000000000003f00000104011100000000",
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
