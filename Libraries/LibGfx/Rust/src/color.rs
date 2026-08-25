/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
#[repr(transparent)]
pub struct Color(pub u32);

impl Color {
    pub const TRANSPARENT: Self = Self(0);
    pub const fn from_rgba(red: u8, green: u8, blue: u8, alpha: u8) -> Self {
        Self((alpha as u32) << 24 | (red as u32) << 16 | (green as u32) << 8 | blue as u32)
    }
    pub const fn from_rgb(red: u8, green: u8, blue: u8) -> Self {
        Self::from_rgba(red, green, blue, 255)
    }
    pub const fn alpha(self) -> u8 {
        (self.0 >> 24) as u8
    }
    pub const fn red(self) -> u8 {
        (self.0 >> 16) as u8
    }
    pub const fn green(self) -> u8 {
        (self.0 >> 8) as u8
    }
    pub const fn blue(self) -> u8 {
        self.0 as u8
    }
    pub const fn with_alpha(self, alpha: u8) -> Self {
        Self((self.0 & 0x00ff_ffff) | (alpha as u32) << 24)
    }
    pub fn blend(self, source: Self) -> Self {
        if self.alpha() == 0 || source.alpha() == 255 {
            return source;
        }
        if source.alpha() == 0 {
            return self;
        }

        let destination_alpha = self.alpha() as u32;
        let source_alpha = source.alpha() as u32;
        let denominator = 255 * (destination_alpha + source_alpha) - destination_alpha * source_alpha;
        let blend_channel = |destination: u8, source: u8| {
            ((destination as u32 * destination_alpha * (255 - source_alpha) + source as u32 * 255 * source_alpha)
                / denominator) as u8
        };
        Self::from_rgba(
            blend_channel(self.red(), source.red()),
            blend_channel(self.green(), source.green()),
            blend_channel(self.blue(), source.blue()),
            (denominator / 255) as u8,
        )
    }
    pub fn with_opacity(self, opacity: f32) -> Self {
        self.with_alpha((self.alpha() as f32 * opacity).round() as u8)
    }
    pub const fn inverted(self) -> Self {
        Self::from_rgba(!self.red(), !self.green(), !self.blue(), self.alpha())
    }
    pub fn interpolate(self, other: Self, weight: f32) -> Self {
        let mix = |a: u8, b: u8| -> u8 {
            let a = a as f32;
            let b = b as f32;
            (a + (b - a) * weight).round_ties_even() as i32 as u8
        };
        Self::from_rgba(
            mix(self.red(), other.red()),
            mix(self.green(), other.green()),
            mix(self.blue(), other.blue()),
            mix(self.alpha(), other.alpha()),
        )
    }
    pub fn mixed_with(self, other: Self, weight: f32) -> Self {
        if self.alpha() == other.alpha() || self.with_alpha(0) == other.with_alpha(0) {
            return self.interpolate(other, weight);
        }
        let alpha = self.alpha() as f32;
        let other_alpha = other.alpha() as f32;
        let mixed_alpha = alpha + (other_alpha - alpha) * weight;
        let premultiplied_mix_channel = |channel: u8, other_channel: u8| -> u8 {
            let a = channel as f32 * alpha;
            let b = other_channel as f32 * other_alpha;
            ((a + (b - a) * weight) / mixed_alpha).round_ties_even() as i32 as u8
        };
        Self::from_rgba(
            premultiplied_mix_channel(self.red(), other.red()),
            premultiplied_mix_channel(self.green(), other.green()),
            premultiplied_mix_channel(self.blue(), other.blue()),
            mixed_alpha.round_ties_even() as i32 as u8,
        )
    }
    pub fn relative_luminance(self) -> f64 {
        fn linearized(component: u8) -> f64 {
            let srgb_component = component as f64 / 255.0;
            if srgb_component <= 0.04045 {
                srgb_component / 12.92
            } else {
                ((srgb_component + 0.055) / 1.055).powf(2.4)
            }
        }
        0.2126 * linearized(self.red()) + 0.7152 * linearized(self.green()) + 0.0722 * linearized(self.blue())
    }
    pub fn contrast_ratio(self, other: Self) -> f64 {
        let l1 = self.relative_luminance();
        let l2 = other.relative_luminance();
        let darkest = l1.min(l2);
        let brightest = l1.max(l2);
        (brightest + 0.05) / (darkest + 0.05)
    }
    pub fn to_hsv(self) -> (f64, f64, f64) {
        let (hue, saturation, value) = srgb_to_hsv(
            self.red() as f32 / 255.0,
            self.green() as f32 / 255.0,
            self.blue() as f32 / 255.0,
        );
        (hue as f64, saturation as f64, value as f64)
    }
    /// Port of Gfx::Color::from_hsv(). The input ranges are the ones the C++ code VERIFYs.
    /// Gfx::hsv_to_srgb() computes in f64 but stores through f32 components, and the result
    /// rounds without clamping (the input ranges keep every product inside 0..=255).
    pub fn from_hsv(hue: f64, saturation: f64, value: f64) -> Self {
        debug_assert!((0.0..360.0).contains(&hue));
        debug_assert!((0.0..=1.0).contains(&saturation));
        debug_assert!((0.0..=1.0).contains(&value));

        let hue = f64::from(hue as f32);
        let saturation = f64::from(saturation as f32);
        let value = f64::from(value as f32);

        let high = ((hue / 60.0) as i32) % 6;
        let f = hue / 60.0 - f64::from(high);
        let c1 = value * (1.0 - saturation);
        let c2 = value * (1.0 - saturation * f);
        let c3 = value * (1.0 - saturation * (1.0 - f));

        let (r, g, b) = match high {
            0 => (value, c3, c1),
            1 => (c2, value, c1),
            2 => (c1, value, c3),
            3 => (c1, c2, value),
            4 => (c3, c1, value),
            5 => (value, c1, c2),
            _ => (0.0, 0.0, 0.0),
        };

        Self::from_rgb(
            (r as f32 * 255.0).round() as u8,
            (g as f32 * 255.0).round() as u8,
            (b as f32 * 255.0).round() as u8,
        )
    }
}

fn srgb_to_hsv(r: f32, g: f32, b: f32) -> (f32, f32, f32) {
    let max = r.max(g).max(b);
    let min = r.min(g).min(b);
    let chroma = max - min;
    let mut hue = 0.0f32;
    if chroma != 0.0 {
        if max == r {
            hue = (60.0 * ((g - b) / chroma)) + 360.0;
        } else if max == g {
            hue = (60.0 * ((b - r) / chroma)) + 120.0;
        } else {
            hue = (60.0 * ((r - g) / chroma)) + 240.0;
        }
    }
    if hue >= 360.0 {
        hue -= 360.0;
    }
    let saturation = if max != 0.0 { chroma / max } else { 0.0 };
    (hue, saturation, max)
}
