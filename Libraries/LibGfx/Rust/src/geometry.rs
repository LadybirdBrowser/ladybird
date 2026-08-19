/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
#[repr(C)]
pub struct IntPoint {
    pub x: i32,
    pub y: i32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct FloatPoint {
    pub x: f32,
    pub y: f32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
#[repr(C)]
pub struct IntSize {
    pub width: i32,
    pub height: i32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct FloatSize {
    pub width: f32,
    pub height: f32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
#[repr(C)]
pub struct IntRect {
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct FloatRect {
    pub x: f32,
    pub y: f32,
    pub width: f32,
    pub height: f32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(i32)]
pub enum Orientation {
    #[default]
    Horizontal,
    Vertical,
}

impl IntRect {
    pub const fn new(x: i32, y: i32, width: i32, height: i32) -> Self {
        Self { x, y, width, height }
    }
    pub const fn is_empty(self) -> bool {
        self.width <= 0 || self.height <= 0
    }
    pub const fn right(self) -> i32 {
        self.x + self.width
    }
    pub const fn bottom(self) -> i32 {
        self.y + self.height
    }
    pub const fn contains_rect(self, other: Self) -> bool {
        self.x <= other.x && self.right() >= other.right() && self.y <= other.y && self.bottom() >= other.bottom()
    }
    pub const fn shrunken(self, top: i32, right: i32, bottom: i32, left: i32) -> Self {
        Self::new(
            self.x + left,
            self.y + top,
            self.width - (left + right),
            self.height - (top + bottom),
        )
    }
    pub const fn inflated(self, w: i32, h: i32) -> Self {
        Self::new(self.x - w / 2, self.y - h / 2, self.width + w, self.height + h)
    }
    pub const fn inflated_edges(self, top: i32, right: i32, bottom: i32, left: i32) -> Self {
        Self::new(
            self.x - left,
            self.y - top,
            self.width + left + right,
            self.height + top + bottom,
        )
    }
    pub fn united(self, other: Self) -> Self {
        if self.is_empty() {
            return other;
        }
        if other.is_empty() {
            return self;
        }
        let left = self.x.min(other.x);
        let top = self.y.min(other.y);
        let right = self.right().max(other.right());
        let bottom = self.bottom().max(other.bottom());
        Self::new(left, top, right - left, bottom - top)
    }
    pub fn intersected(self, other: Self) -> Self {
        let left = self.x.max(other.x);
        let right = self.right().min(other.right());
        let top = self.y.max(other.y);
        let bottom = self.bottom().min(other.bottom());
        if left >= right || top >= bottom {
            return Self::default();
        }
        Self::new(left, top, right - left, bottom - top)
    }
    pub const fn to_float(self) -> FloatRect {
        FloatRect::new(self.x as f32, self.y as f32, self.width as f32, self.height as f32)
    }
}

impl FloatRect {
    pub const fn new(x: f32, y: f32, width: f32, height: f32) -> Self {
        Self { x, y, width, height }
    }
    pub const fn from_array(values: [f32; 4]) -> Self {
        Self {
            x: values[0],
            y: values[1],
            width: values[2],
            height: values[3],
        }
    }
    pub fn inflated(self, width: f32, height: f32) -> Self {
        Self {
            x: self.x - width / 2.0,
            y: self.y - height / 2.0,
            width: self.width + width,
            height: self.height + height,
        }
    }
    pub fn is_empty(self) -> bool {
        self.width <= 0.0 || self.height <= 0.0
    }
    pub fn contains_rect(self, other: Self) -> bool {
        self.x <= other.x && self.right() >= other.right() && self.y <= other.y && self.bottom() >= other.bottom()
    }
    pub fn right(self) -> f32 {
        self.x + self.width
    }
    pub fn bottom(self) -> f32 {
        self.y + self.height
    }
}

pub fn enclosing_int_rect(rect: FloatRect) -> IntRect {
    let x = rect.x.floor();
    let y = rect.y.floor();
    let right = rect.right().ceil();
    let bottom = rect.bottom().ceil();
    IntRect::new(x as i32, y as i32, (right - x) as i32, (bottom - y) as i32)
}
