/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(i32)]
pub enum WindingRule {
    #[default]
    Nonzero,
    EvenOdd,
}

impl WindingRule {
    pub fn from_raw(value: i32) -> Self {
        if value == Self::EvenOdd as i32 {
            Self::EvenOdd
        } else {
            Self::Nonzero
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum LineStyle {
    #[default]
    Solid,
    Dotted,
    Dashed,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(i32)]
pub enum ScalingMode {
    #[default]
    None,
    Bilinear,
    BilinearMipmap,
    NearestNeighbor,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(i32)]
pub enum CompositingAndBlendingOperator {
    #[default]
    Normal = 1,
    Multiply,
    Screen,
    Darken,
    Lighten,
    Overlay,
    ColorDodge,
    ColorBurn,
    HardLight,
    SoftLight,
    Difference,
    Exclusion,
    Hue,
    Saturation,
    Color,
    Luminosity,
    Clear,
    Copy,
    SourceOver,
    DestinationOver,
    SourceIn,
    DestinationIn,
    SourceOut,
    DestinationOut,
    SourceATop,
    DestinationATop,
    Xor,
    Lighter,
    PlusDarker,
    PlusLighter,
}

impl CompositingAndBlendingOperator {
    pub fn from_i32(value: i32) -> Option<Self> {
        if (Self::Normal as i32..=Self::PlusLighter as i32).contains(&value) {
            // SAFETY: the enum is a dense i32 range from Normal to PlusLighter.
            Some(unsafe { std::mem::transmute::<i32, Self>(value) })
        } else {
            None
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(i32)]
pub enum MaskKind {
    #[default]
    Alpha,
    Luminance,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum ShouldAntiAlias {
    #[default]
    Yes = 0,
    No = 1,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(i32)]
pub enum InterpolationColorSpace {
    LinearRGB,
    #[default]
    SRGB,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum RectangularColorSpace {
    Srgb,
    SrgbLinear,
    DisplayP3,
    DisplayP3Linear,
    A98Rgb,
    ProphotoRgb,
    Rec2020,
    Lab,
    #[default]
    Oklab,
    Xyz,
    XyzD50,
    XyzD65,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum PolarColorSpace {
    Hsl,
    Hwb,
    Lch,
    #[default]
    Oklch,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum HueInterpolationMethod {
    #[default]
    Shorter,
    Longer,
    Increasing,
    Decreasing,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum GradientInterpolationType {
    #[default]
    Rectangular,
    Polar,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct GradientInterpolationMethod {
    pub interpolation_type: GradientInterpolationType,
    pub rectangular_color_space: RectangularColorSpace,
    pub polar_color_space: PolarColorSpace,
    pub hue_interpolation_method: HueInterpolationMethod,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(i32)]
pub enum CapStyle {
    #[default]
    Butt,
    Round,
    Square,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(i32)]
pub enum JoinStyle {
    #[default]
    Miter,
    Round,
    Bevel,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(i32)]
pub enum ColorFilterType {
    #[default]
    Brightness,
    Contrast,
    Grayscale,
    Invert,
    Opacity,
    Saturate,
    Sepia,
}

impl ScalingMode {
    pub fn from_i32(value: i32) -> Option<Self> {
        if (Self::None as i32..=Self::NearestNeighbor as i32).contains(&value) {
            // SAFETY: the enum is a dense i32 range starting at zero.
            Some(unsafe { std::mem::transmute::<i32, Self>(value) })
        } else {
            None
        }
    }
}

impl InterpolationColorSpace {
    pub fn from_i32(value: i32) -> Option<Self> {
        match value {
            value if value == Self::LinearRGB as i32 => Some(Self::LinearRGB),
            value if value == Self::SRGB as i32 => Some(Self::SRGB),
            _ => None,
        }
    }
}

impl ColorFilterType {
    pub fn from_i32(value: i32) -> Option<Self> {
        if (Self::Brightness as i32..=Self::Sepia as i32).contains(&value) {
            // SAFETY: the enum is a dense i32 range starting at zero.
            Some(unsafe { std::mem::transmute::<i32, Self>(value) })
        } else {
            None
        }
    }
}
