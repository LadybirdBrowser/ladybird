/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::{CssPixelSize, CssPixels};

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct Fraction {
    pub numerator: CssPixels,
    pub denominator: CssPixels,
}

impl Fraction {
    pub(crate) fn one() -> Self {
        Self {
            numerator: CssPixels::from_integer(1),
            denominator: CssPixels::from_integer(1),
        }
    }

    pub(crate) fn of(numerator: CssPixels, denominator: CssPixels) -> Self {
        Self { numerator, denominator }
    }

    fn to_css_pixels(self) -> CssPixels {
        self.numerator.div_as_fraction(self.denominator)
    }

    fn might_be_saturated(self) -> bool {
        let raw = self.to_css_pixels().raw_value();
        raw == i32::MAX || raw == i32::MIN
    }

    fn to_double(self) -> f64 {
        self.numerator.to_double() / self.denominator.to_double()
    }

    pub(crate) fn multiply(self, value: CssPixels) -> CssPixels {
        let wide = value.raw_value() as i64 * self.numerator.raw_value() as i64;
        CssPixels::from_raw((wide / self.denominator.raw_value() as i64).clamp(i32::MIN as i64, i32::MAX as i64) as i32)
    }

    fn divide(self, value: CssPixels) -> CssPixels {
        let wide = value.raw_value() as i64 * self.denominator.raw_value() as i64;
        CssPixels::from_raw((wide / self.numerator.raw_value() as i64).clamp(i32::MIN as i64, i32::MAX as i64) as i32)
    }

    pub(crate) fn ge(self, other: Self) -> bool {
        let left = self.numerator.raw_value() as i64 * other.denominator.raw_value() as i64;
        let right = other.numerator.raw_value() as i64 * self.denominator.raw_value() as i64;
        left >= right
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(crate) struct SizeWithAspectRatio {
    pub width: Option<CssPixels>,
    pub height: Option<CssPixels>,
    pub aspect_ratio: Option<Fraction>,
}

impl SizeWithAspectRatio {
    pub(crate) fn from_ffi(natural: &crate::painting::host::FfiNaturalSize) -> Self {
        Self {
            width: natural.width.has_value.then_some(natural.width.value),
            height: natural.height.has_value.then_some(natural.height.value),
            aspect_ratio: natural.has_aspect_ratio.then_some(Fraction::of(
                natural.aspect_ratio_numerator,
                natural.aspect_ratio_denominator,
            )),
        }
    }
}

pub(crate) fn run_default_sizing_algorithm(
    specified_width: Option<CssPixels>,
    specified_height: Option<CssPixels>,
    natural: &SizeWithAspectRatio,
    default_size: CssPixelSize,
) -> CssPixelSize {
    if let (Some(width), Some(height)) = (specified_width, specified_height) {
        return CssPixelSize::new(width, height);
    }
    if specified_width.is_some() || specified_height.is_some() {
        if let Some(aspect_ratio) = natural.aspect_ratio
            && !aspect_ratio.might_be_saturated()
        {
            if let Some(width) = specified_width {
                return CssPixelSize::new(width, aspect_ratio.divide(width));
            }
            if let Some(height) = specified_height {
                return CssPixelSize::new(aspect_ratio.multiply(height), height);
            }
        }
        if let (Some(height), Some(natural_width)) = (specified_height, natural.width) {
            return CssPixelSize::new(natural_width, height);
        }
        if let (Some(width), Some(natural_height)) = (specified_width, natural.height) {
            return CssPixelSize::new(width, natural_height);
        }
        if let Some(height) = specified_height {
            return CssPixelSize::new(default_size.width, height);
        }
        if let Some(width) = specified_width {
            return CssPixelSize::new(width, default_size.height);
        }
        unreachable!();
    }
    if natural.width.is_some() || natural.height.is_some() {
        return run_default_sizing_algorithm(natural.width, natural.height, natural, default_size);
    }
    if let Some(aspect_ratio) = natural.aspect_ratio
        && !aspect_ratio.might_be_saturated()
    {
        if default_size.is_empty() {
            return default_size;
        }
        let default_width = default_size.width.to_double();
        let default_height = default_size.height.to_double();
        if aspect_ratio.to_double() >= default_width / default_height {
            return CssPixelSize::new(
                default_size.width,
                CssPixels::nearest_value_for(default_width / aspect_ratio.to_double()),
            );
        }
        return CssPixelSize::new(
            CssPixels::nearest_value_for(default_height * aspect_ratio.to_double()),
            default_size.height,
        );
    }
    default_size
}
