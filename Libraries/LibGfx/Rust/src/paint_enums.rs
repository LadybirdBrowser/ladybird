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

impl ScalingMode {
    pub fn from_raw(value: i32) -> Self {
        if (0..=Self::NearestNeighbor as i32).contains(&value) {
            // SAFETY: the enum is a dense i32 range starting at zero.
            unsafe { std::mem::transmute::<i32, Self>(value) }
        } else {
            Self::default()
        }
    }
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

impl RectangularColorSpace {
    pub fn from_u8(value: u8) -> Self {
        if value <= Self::XyzD65 as u8 {
            // SAFETY: the enum is a dense u8 range starting at zero.
            unsafe { std::mem::transmute::<u8, Self>(value) }
        } else {
            Self::default()
        }
    }
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

impl PolarColorSpace {
    pub fn from_u8(value: u8) -> Self {
        if value <= Self::Oklch as u8 {
            // SAFETY: the enum is a dense u8 range starting at zero.
            unsafe { std::mem::transmute::<u8, Self>(value) }
        } else {
            Self::default()
        }
    }
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

impl HueInterpolationMethod {
    pub fn from_u8(value: u8) -> Self {
        if value <= Self::Decreasing as u8 {
            // SAFETY: the enum is a dense u8 range starting at zero.
            unsafe { std::mem::transmute::<u8, Self>(value) }
        } else {
            Self::default()
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum GradientInterpolationType {
    #[default]
    Rectangular,
    Polar,
}

impl GradientInterpolationType {
    pub fn from_u8(value: u8) -> Self {
        if value <= Self::Polar as u8 {
            // SAFETY: the enum is a dense u8 range starting at zero.
            unsafe { std::mem::transmute::<u8, Self>(value) }
        } else {
            Self::default()
        }
    }
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
