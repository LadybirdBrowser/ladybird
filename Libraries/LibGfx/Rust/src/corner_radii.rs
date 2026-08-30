/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::geometry::{FloatPoint, FloatRect, IntPoint, IntRect};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
#[repr(C)]
pub struct CornerRadius {
    pub horizontal_radius: i32,
    pub vertical_radius: i32,
}

impl CornerRadius {
    pub const fn is_present(self) -> bool {
        self.horizontal_radius > 0 && self.vertical_radius > 0
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
#[repr(C)]
pub struct CornerRadii {
    pub top_left: CornerRadius,
    pub top_right: CornerRadius,
    pub bottom_right: CornerRadius,
    pub bottom_left: CornerRadius,
}

impl CornerRadii {
    pub const fn has_any_radius(self) -> bool {
        self.top_left.is_present()
            || self.top_right.is_present()
            || self.bottom_right.is_present()
            || self.bottom_left.is_present()
    }
    pub const fn uniform(radius: i32) -> Self {
        let corner = CornerRadius {
            horizontal_radius: radius,
            vertical_radius: radius,
        };
        Self {
            top_left: corner,
            top_right: corner,
            bottom_right: corner,
            bottom_left: corner,
        }
    }

    pub fn contains_int_point(self, point: IntPoint, rect: IntRect) -> bool {
        if !rect.contains_point(point) {
            return false;
        }
        if !self.has_any_radius() {
            return true;
        }
        let px = point.x;
        let py = point.y;
        let outside_ellipse = |r: CornerRadius, cx: i32, cy: i32| {
            let dx = (px - cx) as f32 / r.horizontal_radius as f32;
            let dy = (py - cy) as f32 / r.vertical_radius as f32;
            dx * dx + dy * dy > 1.0
        };
        if self.top_left.is_present() {
            let cx = rect.x + self.top_left.horizontal_radius;
            let cy = rect.y + self.top_left.vertical_radius;
            if px < cx && py < cy && outside_ellipse(self.top_left, cx, cy) {
                return false;
            }
        }
        if self.top_right.is_present() {
            let cx = rect.right() - self.top_right.horizontal_radius;
            let cy = rect.y + self.top_right.vertical_radius;
            if px > cx && py < cy && outside_ellipse(self.top_right, cx, cy) {
                return false;
            }
        }
        if self.bottom_right.is_present() {
            let cx = rect.right() - self.bottom_right.horizontal_radius;
            let cy = rect.bottom() - self.bottom_right.vertical_radius;
            if px > cx && py > cy && outside_ellipse(self.bottom_right, cx, cy) {
                return false;
            }
        }
        if self.bottom_left.is_present() {
            let cx = rect.x + self.bottom_left.horizontal_radius;
            let cy = rect.bottom() - self.bottom_left.vertical_radius;
            if px < cx && py > cy && outside_ellipse(self.bottom_left, cx, cy) {
                return false;
            }
        }
        true
    }

    pub fn contains_float_point(self, point: FloatPoint, rect: FloatRect) -> bool {
        if !rect.contains_point(point) {
            return false;
        }
        if !self.has_any_radius() {
            return true;
        }
        let px = point.x;
        let py = point.y;
        let outside_ellipse = |r: CornerRadius, cx: f32, cy: f32| {
            let dx = (px - cx) / r.horizontal_radius as f32;
            let dy = (py - cy) / r.vertical_radius as f32;
            dx * dx + dy * dy > 1.0
        };
        if self.top_left.is_present() {
            let cx = rect.x + self.top_left.horizontal_radius as f32;
            let cy = rect.y + self.top_left.vertical_radius as f32;
            if px < cx && py < cy && outside_ellipse(self.top_left, cx, cy) {
                return false;
            }
        }
        if self.top_right.is_present() {
            let cx = rect.right() - self.top_right.horizontal_radius as f32;
            let cy = rect.y + self.top_right.vertical_radius as f32;
            if px > cx && py < cy && outside_ellipse(self.top_right, cx, cy) {
                return false;
            }
        }
        if self.bottom_right.is_present() {
            let cx = rect.right() - self.bottom_right.horizontal_radius as f32;
            let cy = rect.bottom() - self.bottom_right.vertical_radius as f32;
            if px > cx && py > cy && outside_ellipse(self.bottom_right, cx, cy) {
                return false;
            }
        }
        if self.bottom_left.is_present() {
            let cx = rect.x + self.bottom_left.horizontal_radius as f32;
            let cy = rect.bottom() - self.bottom_left.vertical_radius as f32;
            if px < cx && py > cy && outside_ellipse(self.bottom_left, cx, cy) {
                return false;
            }
        }
        true
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(i32)]
pub enum CornerClip {
    #[default]
    Outside,
    Inside,
}
