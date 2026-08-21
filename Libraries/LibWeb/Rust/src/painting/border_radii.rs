/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::computed_value_views::LengthPercentageRef;
use crate::css::css_pixels::{CssPixelFraction, CssPixels};
use crate::css::css_pixels::{CssPixelPoint, CssPixelRect};
use crate::painting::display_list::device_pixels::DevicePixelConverter;
use libgfx_rust::{CornerRadii, CornerRadius};

pub(crate) fn normalize_border_radii_data(
    border_rect: CssPixelRect,
    reference_rect: CssPixelRect,
    corner_radius_pairs: [(LengthPercentageRef<'_>, LengthPercentageRef<'_>); 4],
) -> BorderRadii {
    scale_radii_to_fit(border_rect, resolve_corner_radii(reference_rect, corner_radius_pairs))
}

pub(crate) fn resolve_corner_radii(
    reference_rect: CssPixelRect,
    corner_radius_pairs: [(LengthPercentageRef<'_>, LengthPercentageRef<'_>); 4],
) -> BorderRadii {
    let mut radii = BorderRadii::default();
    for (corner, (horizontal, vertical)) in corner_radius_pairs.into_iter().enumerate() {
        radii.values[corner * 2] = horizontal.to_px(reference_rect.width);
        radii.values[corner * 2 + 1] = vertical.to_px(reference_rect.height);
    }
    radii
}

pub(crate) fn scale_radii_to_fit(border_rect: CssPixelRect, mut radii: BorderRadii) -> BorderRadii {
    let zero = CssPixels::from_raw(0);
    let border_width = border_rect.width.max(zero);
    let border_height = border_rect.height.max(zero);
    for _iteration in 0..2 {
        let [tl_h, tl_v, tr_h, tr_v, br_h, br_v, bl_h, bl_v] = radii.values;
        let s_top = tl_h + tr_h;
        let s_right = tr_v + br_v;
        let s_bottom = br_h + bl_h;
        let s_left = bl_v + tl_v;

        let mut f = CssPixelFraction::one();
        if s_top > zero && s_top > border_width {
            f = f.min(CssPixelFraction::ratio_of(border_width, s_top));
        }
        if s_right > zero && s_right > border_height {
            f = f.min(CssPixelFraction::ratio_of(border_height, s_right));
        }
        if s_bottom > zero && s_bottom > border_width {
            f = f.min(CssPixelFraction::ratio_of(border_width, s_bottom));
        }
        if s_left > zero && s_left > border_height {
            f = f.min(CssPixelFraction::ratio_of(border_height, s_left));
        }

        if f.is_at_least_one() {
            break;
        }
        for value in &mut radii.values {
            *value = value.mul_by_fraction(f);
        }
    }
    radii
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct BorderRadii {
    pub values: [CssPixels; 8],
}

impl BorderRadii {
    fn corner_present(horizontal: CssPixels, vertical: CssPixels) -> bool {
        horizontal > CssPixels::from_raw(0) && vertical > CssPixels::from_raw(0)
    }

    pub fn shrink(&mut self, top: CssPixels, right: CssPixels, bottom: CssPixels, left: CssPixels) {
        let zero = CssPixels::from_raw(0);
        let shrink_one = |radius: &mut CssPixels, by: CssPixels| {
            if *radius != zero {
                *radius = (*radius - by).max(zero);
            }
        };
        shrink_one(&mut self.values[0], left);
        shrink_one(&mut self.values[1], top);
        shrink_one(&mut self.values[2], right);
        shrink_one(&mut self.values[3], top);
        shrink_one(&mut self.values[4], right);
        shrink_one(&mut self.values[5], bottom);
        shrink_one(&mut self.values[6], left);
        shrink_one(&mut self.values[7], bottom);
    }

    pub fn shrunken(mut self, top: CssPixels, right: CssPixels, bottom: CssPixels, left: CssPixels) -> Self {
        self.shrink(top, right, bottom, left);
        self
    }

    pub fn inflate(&mut self, top: CssPixels, right: CssPixels, bottom: CssPixels, left: CssPixels) {
        self.shrink(-top, -right, -bottom, -left);
    }

    fn corner(&self, index: usize, converter: &DevicePixelConverter) -> CornerRadius {
        CornerRadius {
            horizontal_radius: converter.floored_device_pixels(self.values[index]),
            vertical_radius: converter.floored_device_pixels(self.values[index + 1]),
        }
    }

    pub fn as_corners(&self, converter: &DevicePixelConverter) -> CornerRadii {
        if !self.has_any_radius() {
            return CornerRadii::default();
        }
        self.corners_unconditionally(converter)
    }

    pub fn corners_unconditionally(&self, converter: &DevicePixelConverter) -> CornerRadii {
        CornerRadii {
            top_left: self.corner(0, converter),
            top_right: self.corner(2, converter),
            bottom_right: self.corner(4, converter),
            bottom_left: self.corner(6, converter),
        }
    }

    pub fn has_any_radius(&self) -> bool {
        let [tl_h, tl_v, tr_h, tr_v, br_h, br_v, bl_h, bl_v] = self.values;
        Self::corner_present(tl_h, tl_v)
            || Self::corner_present(tr_h, tr_v)
            || Self::corner_present(br_h, br_v)
            || Self::corner_present(bl_h, bl_v)
    }

    pub fn contains(&self, point: CssPixelPoint, rect: CssPixelRect) -> bool {
        if !rect.contains_point(point) {
            return false;
        }
        if !self.has_any_radius() {
            return true;
        }
        let outside_ellipse =
            |horizontal: CssPixels, vertical: CssPixels, center_x: CssPixels, center_y: CssPixels| -> bool {
                let dx = (point.x - center_x).to_double() / horizontal.to_double();
                let dy = (point.y - center_y).to_double() / vertical.to_double();
                dx * dx + dy * dy > 1.0
            };
        let [tl_h, tl_v, tr_h, tr_v, br_h, br_v, bl_h, bl_v] = self.values;
        if Self::corner_present(tl_h, tl_v) {
            let center_x = rect.left() + tl_h;
            let center_y = rect.top() + tl_v;
            if point.x < center_x && point.y < center_y && outside_ellipse(tl_h, tl_v, center_x, center_y) {
                return false;
            }
        }
        if Self::corner_present(tr_h, tr_v) {
            let center_x = rect.right() - tr_h;
            let center_y = rect.top() + tr_v;
            if point.x > center_x && point.y < center_y && outside_ellipse(tr_h, tr_v, center_x, center_y) {
                return false;
            }
        }
        if Self::corner_present(br_h, br_v) {
            let center_x = rect.right() - br_h;
            let center_y = rect.bottom() - br_v;
            if point.x > center_x && point.y > center_y && outside_ellipse(br_h, br_v, center_x, center_y) {
                return false;
            }
        }
        if Self::corner_present(bl_h, bl_v) {
            let center_x = rect.left() + bl_h;
            let center_y = rect.bottom() - bl_v;
            if point.x < center_x && point.y > center_y && outside_ellipse(bl_h, bl_v, center_x, center_y) {
                return false;
            }
        }
        true
    }
}
