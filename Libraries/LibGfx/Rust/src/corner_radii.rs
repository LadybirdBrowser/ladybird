/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(i32)]
pub enum CornerClip {
    #[default]
    Outside,
    Inside,
}
