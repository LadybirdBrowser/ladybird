/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixels;
use crate::css::css_pixels::{CssPixelPoint, CssPixelRect, CssPixelSize};
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
        IntRect {
            x: to_device_pixels((rect.x.to_double() * scale).floor()),
            y: to_device_pixels((rect.y.to_double() * scale).floor()),
            width: to_device_pixels((rect.width.to_double() * scale).ceil()),
            height: to_device_pixels((rect.height.to_double() * scale).ceil()),
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
