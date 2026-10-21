/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#canvasrenderingcontext2d

use crate::bindings::conversions::CppInterface;
use crate::bindings::{CallContext, WebIdlResult};
use crate::canvas_2d::context::{CanvasKind, OnScreen, RenderingContext2D};

pub(crate) type CanvasRenderingContext2D = RenderingContext2D<OnScreen>;

/// https://html.spec.whatwg.org/multipage/canvas.html#imagesmoothingquality
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ImageSmoothingQuality {
    Low,
    Medium,
    High,
}

/// https://html.spec.whatwg.org/multipage/canvas.html#canvaslinecap
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum CanvasLineCap {
    Butt,
    Round,
    Square,
}

/// https://html.spec.whatwg.org/multipage/canvas.html#canvaslinejoin
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum CanvasLineJoin {
    Round,
    Bevel,
    Miter,
}

/// https://html.spec.whatwg.org/multipage/canvas.html#canvastextalign
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum CanvasTextAlign {
    Start,
    End,
    Left,
    Right,
    Center,
}

/// https://html.spec.whatwg.org/multipage/canvas.html#canvastextbaseline
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum CanvasTextBaseline {
    Top,
    Hanging,
    Middle,
    Alphabetic,
    Ideographic,
    Bottom,
}

/// https://html.spec.whatwg.org/multipage/canvas.html#canvasdirection
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum CanvasDirection {
    Ltr,
    Rtl,
    Inherit,
}

impl<K: CanvasKind> RenderingContext2D<K> {
    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-canvas
    /// https://html.spec.whatwg.org/multipage/canvas.html#offscreencontext2d-canvas
    pub(crate) fn canvas(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        Ok(self.canvas_handle().wrapper(cx.realm()))
    }
}
