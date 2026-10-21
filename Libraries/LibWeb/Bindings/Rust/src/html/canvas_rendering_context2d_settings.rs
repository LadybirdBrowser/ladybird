/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#canvasrenderingcontext2dsettings

use super::predefined_color_space::PredefinedColorSpace;

/// https://html.spec.whatwg.org/multipage/canvas.html#canvascolortype, in the order of
/// Web::Bindings::CanvasColorType, which C++ passes as its underlying value.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum CanvasColorType {
    Unorm8,
    Float16,
}

impl CanvasColorType {
    /// The color type whose underlying value C++ passes, if it is one.
    pub(crate) fn from_u8(value: u8) -> Option<Self> {
        match value {
            0 => Some(Self::Unorm8),
            1 => Some(Self::Float16),
            _ => None,
        }
    }
}

/// https://html.spec.whatwg.org/multipage/canvas.html#canvasrenderingcontext2dsettings
#[derive(Clone, Copy, Debug)]
pub(crate) struct CanvasRenderingContext2DSettings {
    pub(crate) alpha: bool,
    pub(crate) desynchronized: bool,
    pub(crate) color_space: PredefinedColorSpace,
    pub(crate) color_type: CanvasColorType,
    pub(crate) will_read_frequently: bool,
}
