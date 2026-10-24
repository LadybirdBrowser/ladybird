/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#predefinedcolorspace

use libcompositing_rust::display_list::canvas_2d_recorder::ColorSpace;

/// In the order of Web::Bindings::PredefinedColorSpace, which C++ passes as its underlying value.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum PredefinedColorSpace {
    #[default]
    Srgb,
    SrgbLinear,
    DisplayP3,
    DisplayP3Linear,
}

impl PredefinedColorSpace {
    /// The color space whose underlying value C++ passes, if it is one.
    pub(crate) fn from_u8(value: u8) -> Option<Self> {
        match value {
            0 => Some(Self::Srgb),
            1 => Some(Self::SrgbLinear),
            2 => Some(Self::DisplayP3),
            3 => Some(Self::DisplayP3Linear),
            _ => None,
        }
    }
}

impl From<PredefinedColorSpace> for ColorSpace {
    fn from(color_space: PredefinedColorSpace) -> Self {
        match color_space {
            PredefinedColorSpace::Srgb => Self::Srgb,
            PredefinedColorSpace::SrgbLinear => Self::SrgbLinear,
            PredefinedColorSpace::DisplayP3 => Self::DisplayP3,
            PredefinedColorSpace::DisplayP3Linear => Self::DisplayP3Linear,
        }
    }
}
