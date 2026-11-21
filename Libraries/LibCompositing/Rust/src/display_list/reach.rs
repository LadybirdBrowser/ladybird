/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libgfx_rust::{FloatRect, IntRect};

/// The rects a `Reach` can be bounded by.
pub trait RectOfReach: Copy + Default {
    fn united(self, other: Self) -> Self;
    fn intersected(self, other: Self) -> Self;
    fn is_empty(self) -> bool;
}

impl RectOfReach for IntRect {
    fn united(self, other: Self) -> Self {
        IntRect::united(self, other)
    }

    fn intersected(self, other: Self) -> Self {
        IntRect::intersected(self, other)
    }

    fn is_empty(self) -> bool {
        IntRect::is_empty(self)
    }
}

impl RectOfReach for FloatRect {
    fn united(self, other: Self) -> Self {
        FloatRect::united(self, other)
    }

    fn intersected(self, other: Self) -> Self {
        FloatRect::intersected(self, other)
    }

    fn is_empty(self) -> bool {
        FloatRect::is_empty(self)
    }
}

/// Where content can show or a filter can read: nowhere, within a rect, or anywhere at all.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Reach<Rect> {
    #[default]
    Nothing,
    Within(Rect),
    Anywhere,
}

impl<Rect: RectOfReach> Reach<Rect> {
    pub fn united(self, other: Self) -> Self {
        match (self, other) {
            (Self::Nothing, reach) | (reach, Self::Nothing) => reach,
            (Self::Within(rect), Self::Within(other)) => Self::Within(rect.united(other)),
            _ => Self::Anywhere,
        }
    }

    pub fn touches(self, rect: Rect) -> bool {
        match self {
            Self::Nothing => false,
            Self::Within(within) => !within.intersected(rect).is_empty(),
            Self::Anywhere => true,
        }
    }

    pub fn meets(self, other: Self) -> bool {
        match (self, other) {
            (Self::Nothing, _) | (_, Self::Nothing) => false,
            (Self::Within(rect), Self::Within(other)) => !rect.intersected(other).is_empty(),
            (Self::Within(rect), Self::Anywhere) | (Self::Anywhere, Self::Within(rect)) => !rect.is_empty(),
            (Self::Anywhere, Self::Anywhere) => true,
        }
    }

    /// The part of `area` it reaches, which is empty where it reaches none of it.
    pub fn part_of(self, area: Rect) -> Rect {
        match self {
            Self::Nothing => Rect::default(),
            Self::Within(rect) => rect.intersected(area),
            Self::Anywhere => area,
        }
    }

    pub fn limited_to(self, limit: Rect) -> Self {
        match self {
            Self::Nothing => Self::Nothing,
            Self::Within(rect) => Self::Within(rect.intersected(limit)),
            Self::Anywhere => Self::Within(limit),
        }
    }
}
