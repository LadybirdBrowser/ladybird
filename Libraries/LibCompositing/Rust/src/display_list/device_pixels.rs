/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css_pixels::CssPixels;
use crate::css_pixels::{CssPixelPoint, CssPixelRect, CssPixelSize};
use libgfx_rust::{IntPoint, IntRect, IntSize};

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DevicePixelConverter {
    device_pixels_per_css_pixel: f64,
}

#[inline]
fn to_device_pixels(value: f64) -> i32 {
    value as i32
}

impl DevicePixelConverter {
    pub const fn new(device_pixels_per_css_pixel: f64) -> Self {
        Self {
            device_pixels_per_css_pixel,
        }
    }

    pub const fn device_pixels_per_css_pixel(&self) -> f64 {
        self.device_pixels_per_css_pixel
    }

    pub fn rounded_device_pixels(&self, css_pixels: CssPixels) -> i32 {
        to_device_pixels((css_pixels.to_double() * self.device_pixels_per_css_pixel).round())
    }

    pub fn enclosing_device_pixels(&self, css_pixels: CssPixels) -> i32 {
        to_device_pixels((css_pixels.to_double() * self.device_pixels_per_css_pixel).ceil())
    }

    pub fn floored_device_pixels(&self, css_pixels: CssPixels) -> i32 {
        to_device_pixels((css_pixels.to_double() * self.device_pixels_per_css_pixel).floor())
    }

    pub fn rounded_device_point(&self, point: CssPixelPoint) -> IntPoint {
        IntPoint {
            x: to_device_pixels((point.x.to_double() * self.device_pixels_per_css_pixel).round()),
            y: to_device_pixels((point.y.to_double() * self.device_pixels_per_css_pixel).round()),
        }
    }

    pub fn enclosing_device_rect(&self, rect: CssPixelRect) -> IntRect {
        let scale = self.device_pixels_per_css_pixel;
        let x = (rect.x.to_double() * scale).floor();
        let y = (rect.y.to_double() * scale).floor();
        IntRect {
            x: to_device_pixels(x),
            y: to_device_pixels(y),
            width: if rect.width > CssPixels::default() {
                to_device_pixels(((rect.x.to_double() + rect.width.to_double()) * scale).ceil() - x)
            } else {
                0
            },
            height: if rect.height > CssPixels::default() {
                to_device_pixels(((rect.y.to_double() + rect.height.to_double()) * scale).ceil() - y)
            } else {
                0
            },
        }
    }

    pub fn rounded_device_rect(&self, rect: CssPixelRect) -> IntRect {
        let scale = self.device_pixels_per_css_pixel;
        let x = (rect.x.to_double() * scale).round();
        let y = (rect.y.to_double() * scale).round();
        IntRect {
            x: to_device_pixels(x),
            y: to_device_pixels(y),
            width: to_device_pixels(((rect.x.to_double() + rect.width.to_double()) * scale).round() - x),
            height: to_device_pixels(((rect.y.to_double() + rect.height.to_double()) * scale).round() - y),
        }
    }

    pub fn rounded_device_size(&self, size: CssPixelSize) -> IntSize {
        IntSize {
            width: to_device_pixels((size.width.to_double() * self.device_pixels_per_css_pixel).round()),
            height: to_device_pixels((size.height.to_double() * self.device_pixels_per_css_pixel).round()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn enclosing_rect_covers_fractional_far_edges() {
        for (scale, x, y, width, height, expected) in [
            (1.0, 0.75, 0.75, 200.0, 200.0, IntRect::new(0, 0, 201, 201)),
            (1.0, -0.25, -0.25, 200.0, 200.0, IntRect::new(-1, -1, 201, 201)),
            (2.0, 0.75, 0.75, 200.0, 200.0, IntRect::new(1, 1, 401, 401)),
            (1.5, 0.5, 0.5, 10.0, 20.0, IntRect::new(0, 0, 16, 31)),
            (1.0, 10.0, 20.0, 200.0, 100.0, IntRect::new(10, 20, 200, 100)),
            (1.0, 0.75, 0.75, 0.0, 0.0, IntRect::new(0, 0, 0, 0)),
            (1.0, 0.75, 0.75, 0.0, 200.0, IntRect::new(0, 0, 0, 201)),
            (1.0, 0.75, 0.75, -0.25, 200.0, IntRect::new(0, 0, 0, 201)),
        ] {
            let rect = CssPixelRect::new(
                CssPixels::nearest_value_for(x),
                CssPixels::nearest_value_for(y),
                CssPixels::nearest_value_for(width),
                CssPixels::nearest_value_for(height),
            );
            assert_eq!(DevicePixelConverter::new(scale).enclosing_device_rect(rect), expected);
        }
    }
}
